#include <inttypes.h>

typedef uint64_t uint64;
typedef int64_t int64;
typedef uint64 rowcnt_t;
typedef uint64 *selvec_t;
typedef int64 index_t;

__attribute__((annotate("targetclone"))) rowcnt_t
SelectValidBuckets(rowcnt_t selrows, selvec_t srcSelvec, selvec_t dstSelvec, uint64 *entryIdArr) {
  rowcnt_t numValid = 0;

  for (index_t i = 0; i < selrows; i++) {
    index_t ind = srcSelvec[i];
    dstSelvec[numValid] = ind;
    numValid += (entryIdArr[ind] != 0);
  }

  return numValid;
}
