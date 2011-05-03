#include <apt-pkg/fileutl.h>
#include <apt-pkg/acquire-method.h>

#include "connect.h"
#include "rfc2553emu.h"
#include "s3.h"
#include <iostream>
#include <fstream>
#include <unistd.h>

using namespace std;

int main()
{
   setlocale(LC_ALL, "");

   HttpMethod Mth;
   return Mth.Loop();
}
