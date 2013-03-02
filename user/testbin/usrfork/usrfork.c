#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

int
main(int argc, char *argv[])
{
  (void)argc;
  (void)argv;

  int pid = fork();
  int error;
  switch (pid){
    case 0:
        printf("Hello from child\n");
        break;
    case -1:
        printf("ERROR\n");
        return -1;
        break;
    default:
        waitpid(pid, &error, 0);
        if (error){
          printf("Child exited with error: %d\n", error);
        }
        printf("Hello from parent\n");
  }
  return 0;
}
