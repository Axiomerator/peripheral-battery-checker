"""Measure process resources without trimming the working set."""
import ctypes as C
from ctypes import wintypes as W
import time,json,sys
class Memory(C.Structure):
    _fields_=[('cb',W.DWORD),('PageFaultCount',W.DWORD)]+[(n,C.c_size_t) for n in ['PeakWorkingSetSize','WorkingSetSize','QuotaPeakPagedPoolUsage','QuotaPagedPoolUsage','QuotaPeakNonPagedPoolUsage','QuotaNonPagedPoolUsage','PagefileUsage','PeakPagefileUsage','PrivateUsage','PrivateWorkingSetSize','SharedCommitUsage']]
k=C.WinDLL('kernel32');p=C.WinDLL('psapi')
k.OpenProcess.argtypes=[W.DWORD,W.BOOL,W.DWORD];k.OpenProcess.restype=W.HANDLE
k.CloseHandle.argtypes=[W.HANDLE]
k.GetProcessTimes.argtypes=[W.HANDLE,C.POINTER(W.FILETIME),C.POINTER(W.FILETIME),C.POINTER(W.FILETIME),C.POINTER(W.FILETIME)]
p.GetProcessMemoryInfo.argtypes=[W.HANDLE,C.c_void_p,W.DWORD]
handle=k.OpenProcess(0x410,False,int(sys.argv[1]))
if not handle:raise C.WinError()
def cpu():
    fields=[W.FILETIME() for _ in range(4)]
    if not k.GetProcessTimes(handle,*[C.byref(x) for x in fields]):raise C.WinError()
    return sum((x.dwHighDateTime<<32)|x.dwLowDateTime for x in fields[2:])/1e7
def memory():
    m=Memory();m.cb=C.sizeof(m)
    if not p.GetProcessMemoryInfo(handle,C.byref(m),C.sizeof(m)):raise C.WinError()
    return {n:round(getattr(m,n)/1048576,3) for n in ['WorkingSetSize','PrivateUsage','PrivateWorkingSetSize','SharedCommitUsage']}
try:
    before=cpu();start=time.monotonic();time.sleep(10);seconds=time.monotonic()-start
    result={'pid':int(sys.argv[1]),'sample_seconds':round(seconds,3),'cpu_seconds':round(cpu()-before,6),'memory_MiB':memory(),'working_set_trimmed':False}
    text=json.dumps(result,indent=2)
    if len(sys.argv)>2:
        with open(sys.argv[2],'w',encoding='utf-8') as f:f.write(text+'\n')
    print(text)
finally:k.CloseHandle(handle)
