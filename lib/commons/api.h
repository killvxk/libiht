#include <unistd.h>

#include "../../kernel/commons/xplat.h"
#include "../../kernel/commons/xioctl.h"

struct lbr_ioctl_request enable_lbr();
void disalbe_lbr(struct lbr_ioctl_request usr_request);
void dump_lbr(struct lbr_ioctl_request usr_request);
void select_lbr(struct lbr_ioctl_request usr_request);