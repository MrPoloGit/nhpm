/* objects.c - fake NetHack source fixture (docs/testing.md section 17.2). */
#include <stdio.h>

struct fake_object {
    const char *name;
    int weight;
};

static const struct fake_object objects[] = {
    {"dagger", 10},
    {"long sword", 40},
    {"leather armor", 150},
    {"potion", 20},
};

const char *object_name(int index)
{
    if (index < 0 || (size_t)index >= sizeof(objects) / sizeof(objects[0])) {
        return "unknown object";
    }
    return objects[index].name;
}

int object_weight(int index)
{
    if (index < 0 || (size_t)index >= sizeof(objects) / sizeof(objects[0])) {
        return 0;
    }
    return objects[index].weight;
}

void objects_list(void)
{
    size_t i;
    for (i = 0; i < sizeof(objects) / sizeof(objects[0]); i++) {
        printf("%-16s weight %d\n", objects[i].name, objects[i].weight);
    }
}
