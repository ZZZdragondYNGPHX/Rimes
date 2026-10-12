#include <sys/prctl.h>
#include <sys/resource.h>
#include <unistd.h>
#include <signal.h>
// The child dies when its owning Fcitx process exits, including SIGKILL.
int main(int argc,char** argv) {
    if(argc<2) return 2;
    const auto parent=getppid();
    if(prctl(PR_SET_PDEATHSIG,SIGTERM)!=0 || getppid()!=parent || parent==1) return 3;
    (void)setpriority(PRIO_PROCESS,0,5);
    execv(argv[1],argv+1);
    return 4;
}
