__attribute__((annotate("async"))) int min(int a, int b) { return a < b ? a : b; }

// #pragma enable_annotate
// static inline void my_min_impl(int *a, int *b, int *c, int n) {
//   int i;
//   for (i = 0; i < n; i++) {
//     a[i] = (b[i] < c[i]) ? b[i] : c[i];
//   }
// }

// static inline void my_min_targetted(int *a, int *b, int *c, int n) { my_min_impl(a, b, c, n); }

// void my_min(int *a, int *b, int *c, int n) { my_min_targetted(a, b, c, n); }

// #pragma enable_annotate
// __attribute__((target("avx512f"))) int max(int a, int b, int c) {
//   return a > b ? (a > c ? a : c) : b;
// }

// #pragma enable_annotate
// __attribute__((target("default"))) int max(int a, int b, int c) {
//   return a > b ? (a > c ? a : c) : b;
// }

// void my_min(int *a, int *b, int *c, int n) {
//   int i;
//   for (i = 0; i < n; i++) {
//     a[i] = (b[i] < c[i]) ? b[i] : c[i];
//   }
// }

// #if defined(__cplusplus) && __cplusplus >= 201103L
// #define TARGET(arch) [[gnu::target(#arch)]]
// #else
// #define TARGET(arch) __attribute__((target(#arch)))
// #endif

// static inline void my_min_impl(int *a, int *b, int *c, int n) {
//   int i;
//   for (i = 0; i < n; i++) {
//     a[i] = (b[i] < c[i]) ? b[i] : c[i];
//   }
// }

// TARGET(avx512f)
// static inline void my_min_targetted(int *a, int *b, int *c, int n) { my_min_impl(a, b, c, n); }

// TARGET(avx2)
// static inline void my_min_targetted(int *a, int *b, int *c, int n) { my_min_impl(a, b, c, n); }

// TARGET(avx)
// static inline void my_min_targetted(int *a, int *b, int *c, int n) { my_min_impl(a, b, c, n); }

// TARGET(default)
// static inline void my_min_targetted(int *a, int *b, int *c, int n) { my_min_impl(a, b, c, n); }

// void my_min(int *a, int *b, int *c, int n) { my_min_targetted(a, b, c, n); }
