/* Reproduce libva's nested driver loading without requiring a GPU. */
extern void *dlopen(const char *, int);
__attribute__((visibility("default"))) int arc_nested_load_probe(void) {
    return dlopen("libm.so.6", 2 | 256) != (void *)0;
}
