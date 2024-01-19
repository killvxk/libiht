#pragma once
#include "headers.hpp"

namespace k_hook
{
	// SSDT�ص�����
	typedef void(__fastcall* f_call_back)(unsigned int new_proc, unsigned int old_proc);

	// ��ʼ������
	bool initialize(f_call_back ssdt_call_back);

	// ��ʼ���غ�������
	bool start();

	// �������غ�������
	bool stop();
}