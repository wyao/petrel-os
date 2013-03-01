#include <stdio.h>
#include <unistd.h>
#include <errno.h>

int
main(int argc, char *argv[])
{
  (void)argc;
  (void)argv;

  char buf[128];
  if(__getcwd(buf, sizeof(buf)-1)){
    printf("getcwd err: %d\n", errno);
  }
  printf("Initial directory: %s\n", buf);

  if (chdir("testdir") != 0)
    printf("chdir failed: %d\n", errno);
  printf("success!\n");

  if(__getcwd(buf, sizeof(buf)-1)){
    printf("getcwd err: %d\n", errno);
  }
  printf("Changed directory: %s\n", buf);

  return 0;
}
