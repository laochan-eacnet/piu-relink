/* M2: general-dynamic TLS + COPY-relocated data + symbol versions-free exports. */
static __thread int counter;
__thread int tls_public = 7;          /* initialized TLS image */
int shared_table[4] = { 11, 22, 33, 44 };   /* referenced by the non-PIE exe -> R_386_COPY */
const char *shared_name = "libtlsm";

void tls_incr(void) { counter++; }
int tls_get(void) { return counter; }
int *tls_public_addr(void) { return &tls_public; }
/* reads the table through its GOT: must see the exe's copy after COPY interposition */
int table_sum(void) { return shared_table[0] + shared_table[1] + shared_table[2] + shared_table[3]; }
