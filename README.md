an SVM hypervisor for linux implemented as an LKM.

Virtualizes the whole system at runtime and handles vmexits.

what it has:
- NPT with 1:1 identity mapping
- multicore support
- CR3 pinning (KPTI and cr3 switching workaround)
- Stable
- SVME write/read protection

what it does NOT have:
- intel support
- nested virtualization
- vmmcall API


works on ubuntu's 6.17.0-14-generic. should be fine-ish on other kernels since it doesn't do anything super kernel specific.
if you're gonna run this then just make sure you rmmod before powering off.
