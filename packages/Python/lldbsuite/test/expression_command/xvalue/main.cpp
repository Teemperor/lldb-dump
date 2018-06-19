#include <stddef.h>

struct StringRef
{
  const char *data = 0;
};

StringRef foo() { return StringRef(); }

int main(int argc, char const *argv[])
{
  const char *something = foo().data;
  return 0; // Break here
}
