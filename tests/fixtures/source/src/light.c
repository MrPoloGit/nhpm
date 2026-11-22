/* light.c - fake NetHack source fixture (docs/testing.md section 17.2). */
#include <stdio.h>

static int strcmp_fixture(const char *a, const char *b);

static const char *light_sources[] = {
    "candle",
    "lamp",
    "lantern",
};

int light_radius(const char *source)
{
    if (source == NULL) {
        return 0;
    }
    if (strcmp_fixture(source, "candle") == 0) {
        return 1;
    }
    if (strcmp_fixture(source, "lamp") == 0) {
        return 2;
    }
    if (strcmp_fixture(source, "lantern") == 0) {
        return 3;
    }
    return 0;
}

static int strcmp_fixture(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a - *b;
}

void light_describe(const char *source)
{
    int radius = light_radius(source);
    printf("%s lights a radius of %d\n", source, radius);
}

void light_list_sources(void)
{
    size_t i;
    for (i = 0; i < sizeof(light_sources) / sizeof(light_sources[0]); i++) {
        printf("- %s\n", light_sources[i]);
    }
}
