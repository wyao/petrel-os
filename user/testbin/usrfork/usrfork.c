#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

int
main(int argc, char *argv[])
{
  (void)argc;
  (void)argv;

  if (fork() == 0) {
    printf("Hello from child\n");
  }
  else {
    printf("Hello from parent\n");
  }

  return 0;
}
