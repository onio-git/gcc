/* Compare a warm two-instruction loop at pc % 4 == 0 and pc % 4 == 2.  */

	.section .text.onio_split_word_probe,"ax",@progbits
	.option push
	.option norvc
	.globl onio_split_word_probe
	.type onio_split_word_probe, @function
onio_split_word_probe:
	/* Preserve the iteration count and output pointers in caller-saved regs. */
	mv	t4, a0
	mv	t3, a1
	mv	t2, a2

	/* Warm and then measure the naturally aligned loop. */
	la	t5, .Laligned_loop
	li	t0, 64
	jalr	t6, 0(t5)
	mv	t0, t4
	rdcycle t1
	jalr	t6, 0(t5)
	rdcycle t5
	sub	t1, t5, t1
	sw	t1, 0(t3)

	/* Warm and then measure the otherwise identical split-word loop. */
	la	t5, .Lsplit_loop
	li	t0, 64
	jalr	t6, 0(t5)
	mv	t0, t4
	rdcycle t1
	jalr	t6, 0(t5)
	rdcycle t5
	sub	t1, t5, t1
	sw	t1, 0(t2)
	ret

	.p2align 5
.Laligned_loop:
	addi	t0, t0, -1
	bnez	t0, .Laligned_loop
	jr	t6

	/* Same branch-predictor index, but every 32-bit instruction spans words. */
	.p2align 5
	.option push
	.option rvc
	c.nop
	.option pop
.Lsplit_loop:
	addi	t0, t0, -1
	bnez	t0, .Lsplit_loop
	jr	t6

	.size onio_split_word_probe, .-onio_split_word_probe
	.option pop
