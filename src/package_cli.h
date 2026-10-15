#ifndef SATRIA_PACKAGE_CLI_H
#define SATRIA_PACKAGE_CLI_H

/* Dispatch commands documented in design/cli.md. Return true when argv[1]
   is a package-management command and set *status to its exit status. */
int packageCliRun(int argc, const char *argv[], int *status);

#endif
