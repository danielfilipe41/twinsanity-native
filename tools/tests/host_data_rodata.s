        .rodata
        dlabel D_TEST_STRING
        .asciiz "hello"
        enddlabel D_TEST_STRING
        dlabel D_TEST_WORDS
        .word 0x12345678
        .half 0x1234
        .byte 0x42
        .float 1.5
        .align 2
        .space 4
        enddlabel D_TEST_WORDS
        dlabel D_TEST_REF
        .word FUN_TEST_ADD
        enddlabel D_TEST_REF
