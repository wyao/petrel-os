#include <stdio.h>
#include <unistd.h>

int
main(int argc, char *argv[])
{
  (void)argc;
  (void)argv;

  /*
  if (write(1, "STD_OUT\n", 8) != 8){
    return -1;
  }
  */

  printf("\nhello world\n");

  int err = 0;

  err = write(2, "STD_ERR\n", 8);
  printf("err: %d\n",err);

  err = write(2, "STD_ERR\n", 8);
  printf("err: %d\n",err);

  err = write(2, "STD_ERR\n", 8);
  printf("err: %d\n",err);

  if (write(1, "STD_OUT\n", 8) != 8){
    return -1;
  }

  return 0;
}
