#include "CliFile.h"

#include <cstdio>
#include <string>

CliFile::CliFile() {}
CliFile::~CliFile() {}

bool CliFile::readFile(std::string filename, std::string &fileContent) {
    std::ifstream configFile(filename);
    std::string jsonConfig((std::istreambuf_iterator<char>(configFile)),
                                std::istreambuf_iterator<char>());

    fileContent = jsonConfig;
    return true;
}

bool CliFile::writeFile(std::string filename, std::string fileContent) {
  FILE* fd = fopen(filename.c_str(), "w");

  if (fd == NULL) {
    return false;
  }

  fwrite(fileContent.c_str(), fileContent.length(), 1, fd);
  fclose(fd);

  return true;
}
