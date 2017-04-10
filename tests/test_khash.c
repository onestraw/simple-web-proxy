#include "src/khash.h"
KHASH_MAP_INIT_INT(m32, char)      // instantiate structs and methods
int main() {
  int i;
  int absent, is_missing;
  khint_t k;
  khash_t(m32) *h = kh_init(m32);  // allocate a hash table
 
  for(i=1;i<10000; i++){ 
    k = kh_put(m32, h, i, &absent);  // insert a key to the hash table
    if (!absent){
         printf("absent= %u\n", absent);
         kh_del(m32, h, k);
    }
    kh_value(h, k) = 10;             // set the value
  }
  k = kh_get(m32, h, 10);          // query the hash table
  is_missing = (k == kh_end(h));   // test if the key is present
  k = kh_get(m32, h, 5);
  kh_del(m32, h, k);               // remove a key-value pair
  for (k = kh_begin(h); k != kh_end(h); ++k)  // traverse
    if (kh_exist(h, k))            // test if a bucket contains data
      kh_value(h, k) = 1;
  kh_destroy(m32, h);              // deallocate the hash table
  return 0;
}
