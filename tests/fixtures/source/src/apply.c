/* apply.c - fake NetHack source fixture (docs/testing.md section 17.2). */
#include <stdio.h>

static void apply_unknown(void)
{
    printf("You don't know how to apply that.\n");
}

static void apply_key(void)
{
    printf("You unlock the door.\n");
}

static void apply_pick_axe(void)
{
    printf("You dig downward.\n");
}

static void apply_mirror(void)
{
    printf("You see a reflection.\n");
}

void apply_tool(const char *tool)
{
    if (tool == NULL) {
        apply_unknown();
        return;
    }

    switch (tool[0]) {
    case 'k':
        apply_key();
        break;
    case 'p':
        apply_pick_axe();
        break;
    case 'm':
        apply_mirror();
        break;
    default:
        apply_unknown();
        break;
    }
}
