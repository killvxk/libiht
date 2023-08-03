#include <linux/cpumask.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/kprobes.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/uaccess.h>
#include <linux/fs.h>
#include <linux/preempt.h>
#include <linux/ioctl.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/proc_fs.h>
#include <linux/sched.h>
#include <asm/msr.h>
#include <asm/processor.h>

#include "../include/libiht_lkm.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Thomason Zhao");
MODULE_DESCRIPTION("Intel Hardware Trace Library - Linux Kernel Module");

/************************************************
 * Global variables
 ************************************************/

/*
 * Due to differnt kernel version, determine which struct going to use
 */
#ifdef HAVE_PROC_OPS
static struct proc_ops libiht_ops = {
    .proc_open = device_open,
    .proc_release = device_release,
    .proc_read = device_read,
    .proc_write = device_write,
    .proc_ioctl = device_ioctl};
#else
static struct file_operations libiht_ops = {
    .open = device_open,
    .release = device_release
                   .read = device_read,
    .write = device_write,
    .unlocked_ioctl = device_ioctl};
#endif

/*
 * Structures for installing the context switch hooks.
 */
static struct preempt_notifier notifier;
static struct preempt_ops ops = {
    .sched_in = sched_in,
    .sched_out = sched_out};

/*
 * Structures for installing the syscall (fork) hooks.
 */
static struct kprobe kp = {
    .symbol_name = "kernel_clone",
    .pre_handler = pre_fork_handler,
    .post_handler = post_fork_handler};

// static struct lbr_state *lbr_state_list;
// static u64 lbr_capacity;
static spinlock_t lbr_cache_lock;
static struct proc_dir_entry *proc_entry;

static void print_dbg(const char *format, ...)
#ifdef DEBUG_MSG
{
    va_list args;
    va_start(args, format);
    vprintk(format, args);
}
#else
{
    return;
}
#endif

/************************************************
 * LBR helper functions
 *
 * Help to manage LBR stack/registers
 ************************************************/

/*
 * Flush the LBR registers. Caller should ensure this function run on
 * single cpu (by wrapping get_cpu() and put_cpu())
 */
static void flush_lbr(u8 enable)
{
    int i;

    wrmsrl(MSR_LBR_SELECT, 0);
    wrmsrl(MSR_LBR_TOS, 0);

    for (i = 0; i < lbr_capacity; i++)
    {
        wrmsrl(MSR_LBR_NHM_FROM + i, 0);
        wrmsrl(MSR_LBR_NHM_TO + i, 0);
    }

    if (enable)
        wrmsrl(MSR_IA32_DEBUGCTLMSR, DEBUGCTLMSR_LBR);
    else
        wrmsrl(MSR_IA32_DEBUGCTLMSR, 0);
}

/*
 * Store the LBR registers to kernel maintained datastructure
 */
static void get_lbr(u32 pid)
{
    int i;

    struct lbr_state *state = find_lbr_state(pid);
    if (state == NULL)
        return;

    // rdmsrl(MSR_IA32_DEBUGCTLMSR, state.lbr_ctl);
    rdmsrl(MSR_LBR_SELECT, state->lbr_select);
    rdmsrl(MSR_LBR_TOS, state->lbr_tos);

    for (i = 0; i < lbr_capacity; i++)
    {
        rdmsrl(MSR_LBR_NHM_FROM + i, state->entries[i].from);
        rdmsrl(MSR_LBR_NHM_TO + i, state->entries[i].to);
    }
}

/*
 * Write the LBR registers from kernel maintained datastructure
 */
static void put_lbr(u32 pid)
{
    int i;

    struct lbr_state *state = find_lbr_state(pid);
    if (state == NULL)
        return;

    wrmsrl(MSR_LBR_SELECT, state->lbr_select);
    wrmsrl(MSR_LBR_TOS, state->lbr_tos);

    for (i = 0; i < lbr_capacity; i++)
    {
        wrmsrl(MSR_LBR_NHM_FROM + i, state->entries[i].from);
        wrmsrl(MSR_LBR_NHM_TO + i, state->entries[i].to);
    }
}

/*
 * Dump out the LBR registers to kernel message
 */
static void dump_lbr(u32 pid)
{
    int i;
    struct lbr_state *state;

    get_cpu();
    state = find_lbr_state(pid);
    if (state == NULL)
    {
        print_dbg(KERN_INFO "LIBIHT-LKM: find lbr_state failed\n");
        return;
    }

    get_lbr(pid);

    print_dbg(KERN_INFO "PROC_PID:             %d\n", state->pid);
    print_dbg(KERN_INFO "MSR_LBR_SELECT:       0x%llx\n", state->lbr_select);
    print_dbg(KERN_INFO "MSR_LBR_TOS:          %lld\n", state->lbr_tos);

    for (i = 0; i < lbr_capacity; i++)
    {
        print_dbg(KERN_INFO "MSR_LBR_NHM_FROM[%2d]: 0x%llx\n", i, state->entries[i].from);
        print_dbg(KERN_INFO "MSR_LBR_NHM_TO  [%2d]: 0x%llx\n", i, state->entries[i].to);
    }

    print_dbg(KERN_INFO "LIBIHT-LKM: LBR info for cpuid: %d\n", smp_processor_id());

    put_cpu();
}

/*
 * Enable the LBR feature for the current CPU. *info may be NULL (it is required
 * by on_each_cpu()).
 */
static void enable_lbr_wrap(void *info)
{
    enable_lbr();
}

static void enable_lbr(void)
{

    get_cpu();

    print_dbg(KERN_INFO "LIBIHT-LKM: Enable LBR on cpu core: %d...\n", smp_processor_id());

    /* Flush the LBR and enable it */
    flush_lbr(true);

    put_cpu();
}

/*
 * Disable the LBR feature for the current CPU. *info may be NULL (it is required
 * by on_each_cpu()).
 */
static void diable_lbr_wrap(void *info)
{
    disable_lbr();
}

static void disable_lbr(void)
{

    get_cpu();

    print_dbg(KERN_INFO "LIBIHT-LKM: Disable LBR on cpu core: %d...\n", smp_processor_id());

    /* Remove the filter */
    wrmsrl(MSR_LBR_SELECT, 0);

    /* Flush the LBR and disable it */
    flush_lbr(false);

    put_cpu();
}

/************************************************
 * LBR state helper functions
 *
 * Help to manage kernel LBR state datastructure
 ************************************************/

/*
 * Create a new empty LBR state
 */
static struct lbr_state *create_lbr_state(void)
{
    struct lbr_state *state;
    int state_size = sizeof(struct lbr_state) +
                     lbr_capacity * sizeof(struct lbr_stack_entry);

    state = kmalloc(state_size, GFP_KERNEL);
    if (state == NULL)
        return NULL;

    memset(state, 0, state_size);

    return state;
}

/*
 * Insert new LBR state into the back of the list
 */
static void insert_lbr_state(struct lbr_state *new_state)
{
    struct lbr_state *head;

    if (new_state == NULL)
    {
        print_dbg(KERN_INFO "LIBIHT-LKM: Insert new state param is NULL\n");
        return;
    }
    
    head = lbr_state_list;
    if (head == NULL)
    {
        new_state->prev = new_state;
        new_state->next = new_state;
        lbr_state_list = new_state;
    }
    else
    {
        head->prev->next = new_state;
        new_state->prev = head->prev;
        head->prev = new_state;
        new_state->next = head;
    }
}

static void remove_lbr_state(struct lbr_state *old_state)
{
    struct lbr_state *head;
    struct lbr_state *tmp;

    if (old_state == NULL)
    {
        print_dbg(KERN_INFO "LIBIHT-LKM: Remove old state param is NULL\n");
        return;
    }
    
    head = lbr_state_list;
    if (head == NULL)
    {
        print_dbg(KERN_INFO "LIBIHT-LKM: Remove old state list head is NULL\n");
        return;
    }
    
    if (head == old_state)
    {
        // Check if only one state in the list
        lbr_state_list = head->next == head ? NULL : head->next;
    }

    // Unlink from linked list
    // if (old_state->prev != NULL)
    old_state->prev->next = old_state->next;
    
    // if (old_state->next != NULL)
    old_state->next->prev = old_state->prev;

    // Free all its child
    if (lbr_state_list != NULL)
    {
        tmp = lbr_state_list;
        do
        {
            if (tmp->parent == old_state)
                remove_lbr_state(tmp);
            tmp = tmp->prev;
        } while (tmp != lbr_state_list);
    }
    
    kfree(old_state);
}

/*
 * Find the LBR state by given the pid. (Try to do as fast as possiable)
 */
static struct lbr_state *find_lbr_state(u32 pid)
{
    // Perform a backward traversal (typically, newly created processes are
    // more likely to be find)
    struct lbr_state *tmp;

    if (lbr_state_list != NULL)
    {
        /* code */
        tmp = lbr_state_list;
        do {
            if (tmp->pid == pid)
                return tmp;
            tmp = tmp->prev;
        } while (tmp != lbr_state_list);
    }
    
    return NULL;
}

/************************************************
 * Save and Restore the LBR during context switches.
 *
 * Should be done as fast as possiable to minimize the overhead of context switches.
 ************************************************/

/*
 * Save LBR
 */
static void save_lbr(void)
{
    unsigned long lbr_cache_flags;
    struct lbr_state *state;

    // Save when target process being preempted
    state = find_lbr_state(current->pid);
    if (state == NULL)
        return;

    print_dbg(KERN_INFO "LIBIHT-LKM: Leave, saving LBR status for pid: %d\n", current->pid);
    spin_lock_irqsave(&lbr_cache_lock, lbr_cache_flags);
    get_lbr(current->pid);
    spin_unlock_irqrestore(&lbr_cache_lock, lbr_cache_flags);
}

/*
 * Restore LBR
 */
static void restore_lbr(void)
{
    unsigned long lbr_cache_flags;
    struct lbr_state *state;

    // Restore when target process being rescheduled
    state = find_lbr_state(current->pid);
    if (state == NULL)
        return;

    print_dbg(KERN_INFO "LIBIHT-LKM: Enter, restoring LBR status for pid: %d\n", current->pid);
    spin_lock_irqsave(&lbr_cache_lock, lbr_cache_flags);
    put_lbr(current->pid);
    spin_unlock_irqrestore(&lbr_cache_lock, lbr_cache_flags);
}

/************************************************
 * Context switch hook functions
 ************************************************/

/*
 * Entering into scheduler
 */
static void sched_in(struct preempt_notifier *pn, int cpu)
{
    restore_lbr();
}

/*
 * Exiting from scheduler
 */
static void sched_out(struct preempt_notifier *pn, struct task_struct *next)
{
    save_lbr();
}

/************************************************
 * Fork syscall hook functions
 ************************************************/

/*
 * Fork syscall post handler (just before fork() is executed)
 */
static int __kprobes pre_fork_handler(struct kprobe *p, struct pt_regs *regs)
{
    // Reserve for future use
    // print_dbg(KERN_INFO "Process %d is calling fork()\n", current->pid);
    return 0;
}

/*
 * Fork syscall post handler (just after fork() is executed)
 */
static void __kprobes post_fork_handler(struct kprobe *p, struct pt_regs *regs,
                                        unsigned long flags)
{
    struct task_struct *task;
    struct list_head *list;
    struct lbr_state *parent_state, *child_state;

    // Examine parent process
    parent_state = find_lbr_state(current->pid);
    if (parent_state == NULL)
        return;

    print_dbg(KERN_INFO "LIBIHT-LKM: Process %d is calling fork()\n", current->pid);

    // TODO: Fix this
    // task = list_entry(&current->children, struct task_struct, children);
    // print_dbg(KERN_INFO "LIBIHT-LKM: Possiable Child PID %d\n", task->pid);

    // Iterate through children
    // list_for_each(list, &current->children)
    // {
    //     task = list_entry(list, struct task_struct, sibling);
    //     print_dbg(KERN_INFO "LIBIHT-LKM: Child's sibling task ptr 0x%p\n", task);
    //     if (task != NULL)
    //     {
    //         print_dbg(KERN_INFO "LIBIHT-LKM: Child PID %d\n", task->pid);
    //         // child_state = find_lbr_state(task->pid);
    //         // if (child_state == NULL)
    //         // {
    //         //     // Set up trace for new child
    //         //     child_state = create_lbr_state();
    //         //     if (child_state != NULL)
    //         //     {
    //         //         // Copy field from parent (forking lbr_state for child)
    //         //         child_state->lbr_select = parent_state->lbr_select;
    //         //         child_state->pid = task->pid;
    //         //         child_state->parent = parent_state;

    //         //         insert_lbr_state(child_state);
    //         //         print_dbg(KERN_INFO "LIBIHT-LKM: new child %d of parent %d is inserted\n",
    //         //                             child_state->pid, current->pid);
    //         //     }
    //         //     else
    //         //     {
    //         //         print_dbg(KERN_INFO "LIBIHT-LKM: new child_state is NULL, create state failed\n");
    //         //     }
    //         // }
    //         // else
    //         // {
    //         //     print_dbg(KERN_INFO "LIBIHT-LKM: child_state is not NULL, child in the list\n");
    //         // }
    //     }
    //     else
    //     {
    //         print_dbg(KERN_INFO "LIBIHT-LKM: Task is NULL, no child found\n");
    //     }
    // }
}

/************************************************
 * Device hook functions
 *
 * Maintain functionality of the libiht-info helper process
 ************************************************/

/*
 * Hooks for opening the device
 */
static int device_open(struct inode *inode, struct file *filp)
{
    // Reserve for future use
    print_dbg(KERN_INFO "LIBIHT-LKM: Device opened.\n");
    return 0;
}

/*
 * Hooks for releasing the device
 */
static int device_release(struct inode *inode, struct file *filp)
{
    // Reserve for future use
    print_dbg(KERN_INFO "LIBIHT-LKM: Device closed.\n");
    return 0;
}

/*
 * Hooks for reading the device
 */
static ssize_t device_read(struct file *filp, char *buffer, size_t length, loff_t *offset)
{
    print_dbg(KERN_INFO "LIBIHT-LKM: Device read.\n");
    // Dafault for the head of the list
    if (lbr_state_list != NULL)
        dump_lbr(lbr_state_list->pid);
    return 0;
}

/*
 * Hooks for writting the device
 */
static ssize_t device_write(struct file *filp, const char *buf, size_t len, loff_t *off)
{
    // Reserve for future use
    print_dbg(KERN_INFO "LIBIHT-LKM: Device write.\n");
    return 0;
}

/*
 * Hooks for I/O controling the device
 */
static long device_ioctl(struct file *filp, unsigned int ioctl_cmd, unsigned long ioctl_param)
{
    struct lbr_state *state;
    struct ioctl_request request;
    unsigned long ret;

    print_dbg(KERN_INFO "LIBIHT-LKM: Got ioctl argument %#x!", ioctl_cmd);

    // Copy user request
    ret = copy_from_user(&request, (struct ioctl_request *)ioctl_param,
                         sizeof(struct ioctl_request));
    if (ret != 0)
    {
        // Partial copy
        print_dbg(KERN_INFO "LIBIHT-LKM: Remaining size %ld\n", ret);
        return -1;
    }

    print_dbg(KERN_INFO "LIBIHT-LKM: request select bits: %lld", request.lbr_select);
    print_dbg(KERN_INFO "LIBIHT-LKM: request pid: %d", request.pid);

    switch (ioctl_cmd)
    {
    case LIBIHT_LKM_IOC_ENABLE_TRACE:
        print_dbg(KERN_INFO "LIBIHT-LKM: ENABLE_TRACE\n");
        // Enable trace for assigned process
        state = create_lbr_state();
        if (state == NULL)
        {
            print_dbg(KERN_INFO "LIBIHT-LKM: create lbr_state failed\n");
            return -EINVAL;
        }

        // Set the field
        state->lbr_select = request.lbr_select ? request.lbr_select : LBR_SELECT;
        state->pid = request.pid ? request.pid : current->pid;
        state->parent = NULL;

        insert_lbr_state(state);
        get_cpu();
        put_lbr(request.pid);
        put_cpu();
        break;

    case LIBIHT_LKM_IOC_DISABLE_TRACE:
        print_dbg(KERN_INFO "LIBIHT-LKM: DISABLE_TRACE\n");
        // Disable trace for assigned process (and its children)
        state = find_lbr_state(request.pid);
        if (state == NULL)
        {
            print_dbg(KERN_INFO "LIBIHT-LKM: find lbr_state failed\n");
            return -EINVAL;
        }

        remove_lbr_state(state);
        break;

    case LIBIHT_LKM_IOC_DUMP_LBR:
        print_dbg(KERN_INFO "LIBIHT-LKM: DUMP_LBR\n");
        // Dump LBR info for assigned process
        dump_lbr(request.pid);
        break;

    case LIBIHT_LKM_IOC_SELECT_LBR:
        print_dbg(KERN_INFO "LIBIHT-LKM: SELECT_LBR\n");
        // Update the select bits for assigned process
        state = find_lbr_state(request.pid);
        if (state == NULL)
        {
            print_dbg(KERN_INFO "LIBIHT-LKM: find lbr_state failed\n");
            return -EINVAL;
        }

        state->lbr_select = ioctl_param;
        get_cpu();
        put_lbr(request.pid);
        put_cpu();
        break;

    default:
        // Error command code
        print_dbg(KERN_INFO "LIBIHT-LKM: Error ioctl command\n");
        return -EINVAL;
    }

    return 0;
}

/************************************************
 * Kernel module functions
 ************************************************/

static int identify_cpu(void)
{
    unsigned int info;
    unsigned int ext_model;
    unsigned int model;
    unsigned int family;
    register unsigned int eax asm("eax");
    int i;

    asm(
        "push %rbx;"
        "push %rcx;"
        "push %rdx;"
        "movl $1, %eax;"
        "cpuid;"
        "pop %rdx;"
        "pop %rcx;"
        "pop %rbx;");

    info = eax;
    family = (info >> 8) & 0xf;

    // Identify CPU family
    if (family != 6)
        return -1;

    model = (info >> 4) & 0xf;
    ext_model = (info >> 16) & 0xf;
    model = (ext_model << 4) + model;

    // Identify CPU model
    lbr_capacity = -1;
    for (i = 0; i < sizeof(cpu_lbr_maps) / sizeof(cpu_lbr_maps[0]); ++i)
    {
        if (model == cpu_lbr_maps[i].model)
        {
            lbr_capacity = cpu_lbr_maps[i].lbr_capacity;
            break;
        }
    }

    if (lbr_capacity == -1)
    {
        // Model name not found
        return -1;
    }

    print_dbg(KERN_INFO "LIBIHT-LKM: DisplayFamily_DisplayModel - %x_%xH\n", family, model);

    return 0;
}

static int __init libiht_lkm_init(void)
{
    print_dbg(KERN_INFO "LIBIHT-LKM: Initializing...\n");

    // Check availability of the cpu
    print_dbg(KERN_INFO "LIBIHT-LKM: Identifying CPU for LBR availability...\n");
    if (identify_cpu() < 0)
    {
        print_dbg(KERN_INFO "LIBIHT-LKM: Identify CPU failed\n");
        return -1;
    }

    // Create user interactive helper process
    print_dbg(KERN_INFO "LIBIHT-LKM: Creating helper process...\n");
    proc_entry = proc_create("libiht-info", 0666, NULL, &libiht_ops);
    if (proc_entry == NULL)
    {
        print_dbg(KERN_INFO "LIBIHT-LKM: Create proc failed\n");
        return -1;
    }

    // Register kprobe hooks on fork system call
    print_dbg(KERN_INFO "LIBIHT-LKM: Registering system call hooks...\n");
    if (register_kprobe(&kp) < 0)
    {
        print_dbg(KERN_INFO "LIBIHT-LKM: kprobe hook failed\n");
        proc_remove(proc_entry);
        return -1;
    }

    // Init & Register hooks on context switches
    print_dbg(KERN_INFO "LIBIHT-LKM: Initializing & Registering context switch hooks...\n");
    preempt_notifier_init(&notifier, &ops);
    preempt_notifier_inc();
    preempt_notifier_register(&notifier);

    // Enable LBR on each cpu (Not yet set the selection filter bit)
    print_dbg(KERN_INFO "LIBIHT-LKM: Initializing LBR for all %d cpus...\n", num_online_cpus());
    on_each_cpu(enable_lbr_wrap, NULL, 1);

    // Set the state list to NULL after module initialized
    lbr_state_list = NULL;

    print_dbg(KERN_INFO "LIBIHT-LKM: Initialization complete\n");
    return 0;
}

static void __exit libiht_lkm_exit(void)
{
    struct lbr_state *tmp;

    print_dbg(KERN_INFO "LIBIHT-LKM: Exiting...\n");

    // Free the LBR state list
    print_dbg(KERN_INFO "LIBIHT-LKM: Freeing LBR state list...\n");
    if (lbr_state_list != NULL)
    {
        tmp = lbr_state_list;

        do
        {
            kfree(tmp);
            tmp = tmp->prev;
        } while (tmp != lbr_state_list);
    }
    
    // Disable LBR on each cpu
    print_dbg(KERN_INFO "LIBIHT-LKM: Disabling LBR for all %d cpus...\n", num_online_cpus());
    on_each_cpu(diable_lbr_wrap, NULL, 1);

    // Unregister hooks on context switches.
    print_dbg(KERN_INFO "LIBIHT-LKM: Unregistering context switch hooks...\n");
    preempt_notifier_unregister(&notifier);

    // Unregister hooks on fork system call
    print_dbg(KERN_INFO "LIBIHT-LKM: Unregistering system call hooks...\n");
    unregister_kprobe(&kp);

    // Remove the helper process if exist
    print_dbg(KERN_INFO "LIBIHT-LKM: Removing helper process...\n");
    proc_remove(proc_entry);

    print_dbg(KERN_INFO "LIBIHT-LKM: Exit complete\n");
}

module_init(libiht_lkm_init);
module_exit(libiht_lkm_exit);
