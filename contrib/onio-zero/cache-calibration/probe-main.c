/* Standalone wrapper for the cache probes, using the CoreMark board port.  */

typedef unsigned int u32;

extern void start_time(void);
extern void stop_time(void);
extern int ee_printf(const char *, ...);

extern u32 onio_cache_replacement_probe(void);
extern void onio_split_word_probe(u32, volatile u32 *, volatile u32 *);

volatile u32 onio_probe_result[2];

int
main(void)
{
  start_time();
#ifdef ONIO_SPLIT_WORD_PROBE
  onio_split_word_probe(100000, &onio_probe_result[0],
				&onio_probe_result[1]);
#else
  onio_probe_result[0] = onio_cache_replacement_probe();
#endif
  stop_time();

#ifdef ONIO_SPLIT_WORD_PROBE
  ee_printf("aligned cycles: %u\nsplit cycles: %u\n",
	    onio_probe_result[0], onio_probe_result[1]);
#else
  ee_printf("replacement probe cycles: %u\n", onio_probe_result[0]);
#endif
  return 0;
}
