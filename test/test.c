int min(int a, int b) { return a < b ? a : b; }
int max(int a, int b) { return a > b ? a : b; }

__attribute__((annotate("targetclone"))) void my_min(int *a, const int *b, const int *c, int n) {
  int i;
  for (i = 0; i < n; i++) {
    a[i] = (b[i] < c[i]) ? b[i] : c[i];
  }
}

// #include <inttypes.h>

// typedef uint64_t uint64;
// typedef int64_t int64;
// typedef uint64 rowcnt_t;
// typedef uint64 *selvec_t;
// typedef int64 index_t;

// rowcnt_t SelectValidBuckets(rowcnt_t selrows, selvec_t srcSelvec, selvec_t dstSelvec,
//                             uint64 *entryIdArr) {
//     rowcnt_t numValid = 0;

//     for (index_t i = 0; i < selrows; i++) {
//         index_t ind = srcSelvec[i];
//         dstSelvec[numValid] = ind;
//         numValid += (entryIdArr[ind] != 0);
//     }

//     return numValid;
// }
