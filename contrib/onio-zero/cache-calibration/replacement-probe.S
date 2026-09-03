/*
   Board-callable discriminator for a two-way, 2 KiB, 32-byte-line I-cache.

   Each trial visits three fresh lines mapping to one set in this order:

       A, B, A, C, B

   Once the set is full, replacing the predicted (most recently accessed) way
   makes all five accesses miss.  LRU makes the second A a hit and incurs four
   misses.  Thirty-one groups across all 32 sets amplify that one-fill
   difference to 928 line fills after the first group.  Instructions within a
   block stay inside its 32-byte line.  The function returns its own rdcycle
   delta; callers should also time the whole call with their normal harness.
*/

	.section .text.onio_cache_replacement_probe,"ax",@progbits
	.option push
	.option norvc
	.altmacro

	.globl onio_cache_replacement_probe
	.type onio_cache_replacement_probe, @function
onio_cache_replacement_probe:
	rdcycle t5
	li	t4, 0
	j	A0
.Lprobe_finish:
	rdcycle a0
	sub	a0, a0, t5
	ret

	/* Begin A bank on a cache-set boundary.  Each emitted block is one line. */
	.p2align 10

	.macro emit_a id
	.p2align 5
A\id:
	beqz	t4, 1f
	j	C\id
1:
	li	t4, 1
	j	B\id
	.endm

	.macro emit_b id, next
	.p2align 5
B\id:
	li	t3, 1
	beq	t4, t3, 1f
	li	t4, 0
	j	A\next
1:
	li	t4, 2
	j	A\id
	.endm

	.macro emit_b_last id
	.p2align 5
B\id:
	li	t3, 1
	beq	t4, t3, 1f
	li	t4, 0
	j	.Lprobe_finish
1:
	li	t4, 2
	j	A\id
	.endm

	.macro emit_c id
	.p2align 5
C\id:
	j	B\id
	.endm

	.set group_index, 0
	.rept 31
	  .set set_index, 0
	  .rept 32
	    emit_a %(group_index * 32 + set_index)
	    .set set_index, set_index + 1
	  .endr

	  .set set_index, 0
	  .rept 32
	    .set trial_index, group_index * 32 + set_index
	    .if trial_index == (31 * 32 - 1)
	      emit_b_last %trial_index
	    .else
	      emit_b %trial_index, %(trial_index + 1)
	    .endif
	    .set set_index, set_index + 1
	  .endr

	  .set set_index, 0
	  .rept 32
	    emit_c %(group_index * 32 + set_index)
	    .set set_index, set_index + 1
	  .endr
	  .set group_index, group_index + 1
	.endr

	.size onio_cache_replacement_probe, .-onio_cache_replacement_probe
	.option pop
