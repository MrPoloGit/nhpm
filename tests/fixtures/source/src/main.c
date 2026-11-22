/* main.c - fake NetHack source fixture, the "game" itself
 * (docs/testing.md section 17.2). Not a real NetHack file. */
#include "../include/patchlevel.h"

#include <stdio.h>

void light_describe(const char *source);
void objects_list(void);
void apply_tool(const char *tool);

int main(void)
{
    printf("%s %d.%d.%d\n", COPYRIGHT_BANNER_A, VERSION_MAJOR, VERSION_MINOR, PATCHLEVEL);
    light_describe("lamp");
    objects_list();
    apply_tool("pick-axe");
    printf("You enter the dungeon.\n");
    return 0;
}
