# Índice do PDA

Nota de teste do renderizador markdown (M4.10). Abra pelo app **Notas** ou
pelo **Arquivos** — `.md` abre direto em modo **Ler**; o botão **Editar**
mostra o texto cru com cursor.

## Seção 2 (h2)

Texto normal com acentos: ação, coração, três pássaros voando à mercê do
vento — e números 0123456789.

### Seção 3 (h3)

- item de lista 1
  - sub-item recuado (2 espaços)
    - nível 2 (4 espaços)
- item de lista 2
* item com asterisco
- [ ] item não checado
  - [x] tarefa aninhada
- [x] item checado

> Citação em bloco,
> linha continuada vira parágrafo normal (subset de bloco).

```lua
-- cerca de código: mono + verde
pda.log("olá do bloco code")
```

---

Parágrafo após o separador horizontal. **Negrito inline** e `código inline`
funcionam no modo leitura (M4.10), e a crase sozinha ` aparece no texto e no
teclado virtual (123 -> fileira 3). Linhas em branco entre blocos NÃO viram
régua: só `---` ou `***` desenham o separador. Listas aninhadas (M4.11):
cada 2 espaços de recuo = 1 nível de bullet, teto em 3 níveis; dentro de
cercas ``` o recuo continua sendo código.

#título-sem-espaço também vira h1 (parser tolerante)
