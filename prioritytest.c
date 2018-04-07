#include "types.h"
#include "stat.h"
#include "user.h"

/* CS550 ATTENTION: to ensure correct compilation of the base code, 
   stub functions for the system call user space wrapper functions are provided. 
   REMEMBER to disable the stub functions (by commenting the following macro) to 
   allow your implementation to work properly. */
//#define STUB_FUNCS

int 
main(int argc, char * argv[])
{
    //printf(1, "BYE~\n");
	//setrunningticks(1000);
	int pid = fork();
	if(pid==0){

	}else{
		int pid1 = fork();
		if(pid1==0){

		}
		else{
			int pid2 = fork();
			if(pid2==0){
				setpriority(getpid(),0);
			}
		}
		
	}
	
	
	
    //shutdown();
    wait();
    wait();
    wait();
    exit(); //return 0;
}
