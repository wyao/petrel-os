#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

int
main(int argc, char *argv[])
{
  (void)argc;
  (void)argv;

  printf("\nhello world\n");

  if (write(1, "STD_OUT\n", 8) != 8){
    return -1;
  }

  if (write(2, "STD_ERR\n", 8) != 8){
    return -1;
  }

  // Test write()
  int fd = open("/usrtest.txt", O_WRONLY | O_CREAT);
  if (fd < 0) {
    write(2, "open failed\n", 12);
    return -1;
  }
  printf("file pointer %d\n opened for writing\n", fd);
  if (write(fd, "This will be output to testfile.txt\n", 36) != 36) {
      write(2, "There was an error writing to testfile.txt\n", 43);
      return -1;
  }
  if (close(fd) != 0){
    write(2, "close failed\n", 13);
    return -1;
  }

  // Test read()
  fd = open("/usrtest.txt", O_RDONLY);
  if (fd < 0) {
    write(2, "open2 failed\n", 13);
    return -1;
  }
  printf("file pointer %d\n opened for reading\n", fd);
  char buffer[4];
  while (read(fd, buffer, 4) != 0){
    printf("%s", buffer);
  }
  printf("Print successful!\n");
  if (close(fd) != 0){
    write(2, "close failed\n", 13);
    return -1;
  }

  return 0;
}
