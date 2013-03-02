#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <err.h>

int
main(int argc, char *argv[])
{
  (void)argc;
  (void)argv;
  /*
  printf("\nhello world\n");

  if (write(1, "STD_OUT\n", 8) != 8){
    return -1;
  }

  if (write(2, "STD_ERR\n", 8) != 8){
    return -1;
  }
  */
  // Test write()
  int fd = open("/usrtest.txt", O_WRONLY | O_CREAT);
  if (fd < 0) {
    err(-1, "open");
  }
  if (write(fd, "This will be output to testfile.txt\n", 36) != 36) {
    err(-1, "write");
  }
  if (close(fd) != 0){
    err(-1, "close");
  }

  // Test read()
  fd = open("/usrtest.txt", O_RDONLY);
  if (fd < 0) {
    err(-1, "open2");
  }
  char buffer[4];
  while (read(fd, buffer, 4) != 0){
    printf("%s", buffer);
  }
  printf("Print successful!\n");
  if (close(fd) != 0){
    err(-1, "close2");
  }

  // Test lseek()
  fd = open("/usrtest.txt", O_RDONLY);
  if (fd < 0) {
    err(-1, "open3");
  }
  while (read(fd, buffer, 4) != 0){
    printf("%s", buffer);

    // Test
    if (lseek(fd, 1, SEEK_CUR) < 0)
      err(-1, "lseek");
  }
  if (close(fd) != 0){
    err(-1, "close3");
  }

  return 0;
}
