#include <stdio.h>
#include <stdlib.h>
#include <unistd.h> /* for fork */
#include <sys/types.h> /* for pid_t */
#include <sys/wait.h> /* for wait */

int main()
{
    /*Spawn a child to run the program.*/
    pid_t pid=fork();
    int status;
    if (pid==0) { /* child process */
        //static char *argv[3]={ (char *)"echo", (char *)"Foo is my name.",NULL};
        static char *argv[3] = {(char*)"argtest", (char*)"test!",NULL};
        execv("/testbin/argtest",argv);
        exit(127); /* only if execv fails */
    }
    else { /* pid!=0; parent process */
        waitpid(pid,&status,0); /* wait for child to exit */
        printf("Child finished\n");
    }
    return 0;
}
