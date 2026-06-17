;; DFA pipeline model for the ONiO.zero ("Aldebaran") core.
;; Copyright (C) 2026 Free Software Foundation, Inc.

;; This file is part of GCC.

;; GCC is free software; you can redistribute it and/or modify it
;; under the terms of the GNU General Public License as published
;; by the Free Software Foundation; either version 3, or (at your
;; option) any later version.

;; GCC is distributed in the hope that it will be useful, but WITHOUT
;; ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
;; or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public
;; License for more details.

;; You should have received a copy of the GNU General Public License
;; along with GCC; see the file COPYING3.  If not see
;; <http://www.gnu.org/licenses/>.

;; ONiO.zero is a single-issue, in-order rv32imc core.  The key point of this
;; model is that loads and multiplies are SINGLE CYCLE (the core has a 1-cycle
;; multiplier and 1-cycle SRAM with no data cache), unlike the shared "generic"
;; model which assumes load=3 and mul=10.  Telling the scheduler the real
;; (cheap) latencies stops it hoisting loads/muls to "hide" latency that does
;; not exist -- hoisting lengthens value live-ranges, inflates register
;; pressure, and on rv32imc spills past the 8 RVC-compressible registers,
;; bloating hot code and the 2 KB I-cache.  Divide is the only multi-cycle
;; operation (2-33 cycles, operand-dependent; modelled at the ~16-cycle mean)
;; and runs on a separate non-pipelined unit.

(define_automaton "onio_zero")

;; One in-order integer pipe (single issue) plus a non-pipelined divider.
(define_cpu_unit "onio_zero_pipe" "onio_zero")
(define_cpu_unit "onio_zero_div" "onio_zero")

(define_insn_reservation "onio_zero_alu" 1
  (and (eq_attr "tune" "onio_zero")
       (eq_attr "type" "unknown,const,arith,shift,slt,multi,auipc,nop,logical,\
			move,bitmanip,min,max,minu,maxu,clz,ctz,rotate,atomic,\
			condmove,crypto,mvpair,zicond"))
  "onio_zero_pipe")

(define_insn_reservation "onio_zero_load" 1
  (and (eq_attr "tune" "onio_zero")
       (eq_attr "type" "load,fpload"))
  "onio_zero_pipe")

(define_insn_reservation "onio_zero_store" 1
  (and (eq_attr "tune" "onio_zero")
       (eq_attr "type" "store,fpstore"))
  "onio_zero_pipe")

(define_insn_reservation "onio_zero_branch" 1
  (and (eq_attr "tune" "onio_zero")
       (eq_attr "type" "branch,jump,call,jalr,ret,trap"))
  "onio_zero_pipe")

;; Single-cycle hardware multiplier.
(define_insn_reservation "onio_zero_imul" 1
  (and (eq_attr "tune" "onio_zero")
       (eq_attr "type" "imul,clmul,cpop"))
  "onio_zero_pipe")

;; Operand-dependent divider, 2-33 cycles; non-pipelined, modelled at the mean.
(define_insn_reservation "onio_zero_idiv" 16
  (and (eq_attr "tune" "onio_zero")
       (eq_attr "type" "idiv"))
  "onio_zero_div*16")

;; Soft-float only (no F extension): map any stray FP/xfer insns onto the pipe
;; so every insn type has a reservation.  These never appear in practice.
(define_insn_reservation "onio_zero_xfer" 1
  (and (eq_attr "tune" "onio_zero")
       (eq_attr "type" "mfc,mtc,fcvt,fcvt_i2f,fcvt_f2i,fmove,fcmp,sfb_alu"))
  "onio_zero_pipe")

(define_insn_reservation "onio_zero_fp" 5
  (and (eq_attr "tune" "onio_zero")
       (eq_attr "type" "fadd,fmul,fmadd,fdiv,fsqrt"))
  "onio_zero_pipe")
