set noexpandtab
set tabstop=4
set shiftwidth=0

let g:syntastic_asm_compiler_options = '-m32 -x assembler-with-cpp'
let g:syntastic_asm_dialect = 'att'

let g:syntastic_c_compiler_options = '-m32 -Wall -Werror -Wno-unused -Wno-format -fno-builtin'
