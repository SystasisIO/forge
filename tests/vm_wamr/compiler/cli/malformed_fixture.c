#include <stdio.h>

int main(int argc, char **argv)
{
   const unsigned char bytes[] = {0, 0x61, 0x73, 0x6d, 1, 0, 0, 0, 1, 1, 255};
   if (argc != 2)
      return 1;
   FILE *output = fopen(argv[1], "wb");
   if (!output)
      return 1;
   const int complete = fwrite(bytes, 1, sizeof(bytes), output) == sizeof(bytes);
   return fclose(output) == 0 && complete ? 0 : 1;
}
