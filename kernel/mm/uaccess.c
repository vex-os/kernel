#include <vex/errno.h>
#include <vex/process.h>
#include <vex/thread.h>
#include <vex/uaccess.h>
#include <vex/vms.h>

ssize_t user_copyin(void* restrict dst, user_addr_t src, size_t size)
{
    thread_t* thread = this_thread();
    process_t* process = thread->process;

    if(process == nullptr) {
        return -EFAULT;
    }

    return vms_copyin(dst, process->vmspace, src.value, size);
}

ssize_t user_copyin_str(char* restrict dst, user_addr_t src, size_t max_sz)
{
    thread_t* thread = this_thread();
    process_t* process = thread->process;

    if(process == nullptr) {
        return -EFAULT;
    }

    return vms_copyin_str(dst, process->vmspace, src.value, max_sz);
}

ssize_t user_copyout(user_addr_t dst, const void* restrict src, size_t size)
{
    thread_t* thread = this_thread();
    process_t* process = thread->process;

    if(process == nullptr) {
        return -EFAULT;
    }

    return vms_copyout(process->vmspace, dst.value, src, size);
}

ssize_t user_copyout_str(user_addr_t dst, const char* restrict src, size_t max_sz)
{
    thread_t* thread = this_thread();
    process_t* process = thread->process;

    if(process == nullptr) {
        return -EFAULT;
    }

    return vms_copyout_str(process->vmspace, dst.value, src, max_sz);
}
