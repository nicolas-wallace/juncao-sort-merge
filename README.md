# Junção Sort-Merge

## Descrição

Implementação do operador de junção **Sort-Merge Join (SMJ)** em C++, conforme especificado no Trabalho III da disciplina CK0095 - Sistemas de Bancos de Dados (2026-1).

O operador realiza a junção entre as tabelas `Grapes` e `Wines` pelo atributo:
- `Grapes.chave_primaria = Wines.chave_estrangeira`

As tabelas originais **não são modificadas** durante a operação.

---

## Estrutura do Projeto

```
.
├── src/
│   └── main.cpp   # Código-fonte completo da implementação
├── data/
│   ├── grapes.csv # Tabela Grapes (entrada)
│   └── wines.csv  # Tabela Wines (entrada)
├── smj            # Executável compilado
└── README.md      # Este arquivo
```

---

## Parâmetros do Sistema

| Parâmetro            | Valor |
|----------------------|-------|
| Frames no buffer (B) | 5     |
| Tuplas por página    | 12    |
| Buffers de entrada   | 4     |
| Buffer de saída      | 1     |

---

## Algoritmo Implementado

### 1. Ordenação Externa (External Sort)

#### Fase 1 — Geração de Runs Iniciais
- Lê até **B = 5 páginas** por vez do disco simulado para o buffer em memória.
- Ordena todas as tuplas carregadas em memória pelo atributo de junção.
- Grava a sequência ordenada como uma *run* no disco simulado.
- Repete até que todas as páginas da tabela tenham sido processadas.

#### Fase 2 — Intercalação Externa
- Enquanto houver mais de 1 run:
  - Agrupa até **4 runs** (MAX_INPUT_FRAMES = B - 1).
  - Executa um k-way merge usando 4 buffers de entrada e 1 buffer de saída.
  - Quando o buffer de saída atinge capacidade máxima (12 tuplas), grava no disco.
  - Gera runs maiores até restar uma única run ordenada.

### 2. Merge Join
- Com ambas as tabelas ordenadas e gravadas no disco simulado, percorre sequencialmente as duas tabelas.
- Compara os atributos de junção:
  - Se `Grapes.key < Wines.key`: avança em Grapes.
  - Se `Grapes.key > Wines.key`: avança em Wines.
  - Se `Grapes.key == Wines.key`: encontra todos os pares com essa chave em ambas as tabelas e produz o produto cartesiano do grupo (todas as combinações).
- As tuplas resultado são acumuladas no buffer de saída e gravadas em páginas.

---

## Estruturas de Dados

| Estrutura | Descrição |
|-----------|-----------|
| `Tuple`   | Vetor de strings com os valores das colunas |
| `Page`    | Array de até 12 tuplas (1 frame de buffer) |
| `Run`     | Vetor de páginas ordenadas (disco simulado) |
| `Table`   | Vetor de páginas + esquema |
| `Schema`  | Mapeamento nome_coluna → índice |

---

## Como Compilar

Requisito: compilador g++ com suporte a C++17.

```bash
g++ -std=c++17 -O2 -Wall -o smj main.cpp
```

---

## Como Executar

Com os arquivos CSV no mesmo diretório:

```bash
./smj
```

Ou especificando os caminhos dos arquivos:

```bash
./smj grapes.csv wines.csv
```

---

## Saída do Programa

O programa exibe:
1. Log detalhado de cada etapa (runs geradas, passagens de intercalação, Merge Join).
2. Resultados dos testes de validação.
3. As primeiras 20 tuplas da tabela resultado.
4. Resumo final (total de tuplas e páginas).

### Exemplo de saída (extrato):

```
[External Sort] Tabela: 'Wines', coluna: 'chave_estrangeira'
  43 páginas, 511 tuplas
  [Fase 1] Gerando runs iniciais: 9 runs
  [Fase 2] Passagem 1: 9 runs -> 3 runs
  [Fase 2] Passagem 2: 3 runs -> 1 run

[Merge Join] Concluído: 510 tuplas resultado em 43 páginas.

=== Testes de Validação ===
[TESTE 1] Chaves de junção iguais em todas as tuplas: PASSOU
[TESTE 2] Consistência de páginas (1-12 tuplas/página): PASSOU
[TESTE 3] Resultado não vazio: PASSOU
[TESTE 4] Número de tuplas no resultado: 510
[TESTE 5] Número de páginas no resultado: 43
```

---

## Observações

- A implementação simula o disco por meio de vetores de páginas alocados em memória.
- A gestão de buffer é respeitada: em nenhum momento mais de B=5 páginas são carregadas simultaneamente.
- As tabelas originais (`grapes` e `wines`) não são alteradas em nenhuma etapa.
- O código está comentado em português para facilitar a verificação.
