/* Private C continuation switch, using the unchanged X32 C callee-save ABI.
 * CALL supplies PC; user trap registers remain in the existing176B context. */
.text
.globl x32_kernel_switch
x32_kernel_switch:
push EBX
push EBP
push E16
push E17
push E18
push E19
push E20
push E21
push E22
push E23
push E24
push E25
push E26
push E27
push E28
push E29
mov dword [EDI],ESP
mov ESP,dword [ESI]
pop E29
pop E28
pop E27
pop E26
pop E25
pop E24
pop E23
pop E22
pop E21
pop E20
pop E19
pop E18
pop E17
pop E16
pop EBP
pop EBX
ret
