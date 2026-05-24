/*
 * Sistemas de Bancos de Dados - 2026-1
 * Trabalho III - Junção Sort-Merge
 *
 * Implementação do operador Sort-Merge Join com:
 *   - Ordenação externa (External Sort) com B=5 frames
 *   - Buffer de 5 frames (cada frame = 1 página = 12 tuplas)
 *   - Fase 1: geração de runs ordenadas (lendo B páginas por vez)
 *   - Fase 2: intercalação externa com até 4 buffers de entrada + 1 de saída
 *   - Merge Join com tratamento de duplicatas (produto cartesiano no grupo)
 */

#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <map>
#include <algorithm>
#include <sstream>
#include <cassert>

// ============================================================
// Constantes do sistema
// ============================================================
const int TUPLES_PER_PAGE  = 12; // tuplas por página
const int BUFFER_FRAMES    = 5;  // B = 5 frames disponíveis no buffer
const int MAX_INPUT_FRAMES = BUFFER_FRAMES - 1; // 4 buffers de entrada na intercalação

// ============================================================
// Estruturas de dados
// ============================================================

/**
 * Esquema de uma tabela: guarda o nome e índice de cada coluna.
 */
struct Schema {
    int num_cols = 0;
    std::vector<std::string> col_names;
    std::map<std::string, int> name_to_index;

    /** Retorna o índice da coluna pelo nome, ou -1 se não existir. */
    int getIndex(const std::string& col_name) const {
        auto it = name_to_index.find(col_name);
        return (it == name_to_index.end()) ? -1 : it->second;
    }

    /** Adiciona uma coluna ao esquema. */
    void addCol(const std::string& name) {
        name_to_index[name] = num_cols++;
        col_names.push_back(name);
    }
};

/**
 * Tupla: vetor de strings com os valores de cada coluna.
 */
struct Tuple {
    std::vector<std::string> cols;

    /** Retorna o valor da coluna pelo índice. */
    std::string getCol(int idx) const {
        if (idx < 0 || idx >= (int)cols.size()) return "";
        return cols[idx];
    }
};

/**
 * Página: contém até TUPLES_PER_PAGE tuplas.
 * Representa um frame no disco simulado.
 */
struct Page {
    Tuple tuples[TUPLES_PER_PAGE];
    int num_tuples = 0;

    bool isFull()  const { return num_tuples >= TUPLES_PER_PAGE; }
    bool isEmpty() const { return num_tuples == 0; }

    void addTuple(const Tuple& t) {
        if (!isFull()) tuples[num_tuples++] = t;
    }

    void clear() { num_tuples = 0; }
};

/**
 * Run: sequência ordenada de páginas no disco simulado.
 * Usada durante a ordenação externa.
 */
typedef std::vector<Page> Run;

/**
 * Tabela: conjunto de páginas + esquema.
 */
struct Table {
    std::vector<Page> pages;
    Schema schema;

    int num_pages() const { return (int)pages.size(); }

    int totalTuples() const {
        int n = 0;
        for (const auto& p : pages) n += p.num_tuples;
        return n;
    }
};

// ============================================================
// Parsing de CSV (suporta campos entre aspas com vírgulas internas)
// ============================================================

/**
 * Parseia uma linha CSV respeitando campos entre aspas.
 * Trata aspas duplas ("") como escape de aspas dentro de um campo.
 */
std::vector<std::string> parseCSVLine(const std::string& line) {
    std::vector<std::string> fields;
    std::string field;
    bool in_quotes = false;

    for (int i = 0; i < (int)line.size(); i++) {
        char c = line[i];
        if (c == '"') {
            // Aspas duplas dentro de campo citado = aspas literal
            if (in_quotes && i + 1 < (int)line.size() && line[i + 1] == '"') {
                field += '"';
                i++;
            } else {
                in_quotes = !in_quotes;
            }
        } else if (c == ',' && !in_quotes) {
            fields.push_back(field);
            field.clear();
        } else {
            field += c;
        }
    }
    fields.push_back(field); // último campo
    return fields;
}

/**
 * Lê um arquivo CSV e retorna uma Table com as tuplas organizadas em páginas.
 * A primeira linha é tratada como cabeçalho (esquema).
 */
Table loadCSV(const std::string& filename) {
    Table table;
    std::ifstream file(filename);

    if (!file.is_open()) {
        std::cerr << "[ERRO] Não foi possível abrir: " << filename << std::endl;
        return table;
    }

    std::string line;
    bool first_line = true;
    Page current_page;

    while (std::getline(file, line)) {
        // Remove \r (Windows line endings)
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;

        auto fields = parseCSVLine(line);

        if (first_line) {
            // Linha de cabeçalho: monta o esquema
            for (const auto& name : fields)
                table.schema.addCol(name);
            first_line = false;
        } else {
            // Linha de dados: cria uma tupla
            Tuple t;
            t.cols = fields;
            // Preenche colunas faltantes
            while ((int)t.cols.size() < table.schema.num_cols)
                t.cols.push_back("");

            if (current_page.isFull()) {
                table.pages.push_back(current_page);
                current_page.clear();
            }
            current_page.addTuple(t);
        }
    }

    // Salva última página parcialmente preenchida
    if (!current_page.isEmpty())
        table.pages.push_back(current_page);

    return table;
}

// ============================================================
// Utilitários de conversão
// ============================================================

/**
 * Extrai todas as tuplas de uma sequência de páginas (Run).
 */
std::vector<Tuple> runToTuples(const Run& run) {
    std::vector<Tuple> result;
    for (const auto& page : run)
        for (int i = 0; i < page.num_tuples; i++)
            result.push_back(page.tuples[i]);
    return result;
}

/**
 * Converte um vetor de tuplas em páginas (Run).
 */
Run tuplesToPages(const std::vector<Tuple>& tuples) {
    Run pages;
    Page current;
    for (const auto& t : tuples) {
        if (current.isFull()) {
            pages.push_back(current);
            current.clear();
        }
        current.addTuple(t);
    }
    if (!current.isEmpty()) pages.push_back(current);
    return pages;
}

// ============================================================
// Fase 1 da Ordenação Externa: geração de runs iniciais
// ============================================================

/**
 * Fase 1 do External Sort: lê B páginas por vez do disco simulado,
 * ordena em memória e grava como uma run ordenada.
 *
 * @param table       Tabela de entrada (não modificada)
 * @param col_idx     Índice da coluna de ordenação
 * @param table_name  Nome para log
 * @return            Vetor de runs ordenadas
 */
std::vector<Run> createInitialRuns(const Table& table, int col_idx,
                                   const std::string& table_name) {
    std::vector<Run> runs;
    int total_pages = table.num_pages();

    std::cout << "  [Fase 1] Gerando runs iniciais para '" << table_name
              << "' (" << total_pages << " páginas, " << BUFFER_FRAMES
              << " frames por run)..." << std::endl;

    // Lê B páginas de cada vez
    for (int page_start = 0; page_start < total_pages; page_start += BUFFER_FRAMES) {
        // === Simula carregamento de até B páginas no buffer ===
        std::vector<Tuple> buffer_tuples;

        int page_end = std::min(page_start + BUFFER_FRAMES, total_pages);
        for (int p = page_start; p < page_end; p++) {
            const Page& page = table.pages[p];
            for (int i = 0; i < page.num_tuples; i++)
                buffer_tuples.push_back(page.tuples[i]);
        }

        // Ordena o conteúdo do buffer em memória
        std::sort(buffer_tuples.begin(), buffer_tuples.end(),
                  [&](const Tuple& a, const Tuple& b) {
                      return a.getCol(col_idx) < b.getCol(col_idx);
                  });

        // Grava run ordenada no disco simulado
        Run run = tuplesToPages(buffer_tuples);
        runs.push_back(run);

        std::cout << "    Run " << runs.size() << ": páginas ["
                  << page_start << ", " << page_end - 1 << "] -> "
                  << run.size() << " páginas, "
                  << buffer_tuples.size() << " tuplas" << std::endl;
    }

    return runs;
}

// ============================================================
// Fase 2 da Ordenação Externa: intercalação externa de runs
// ============================================================

/**
 * Intercala um grupo de até MAX_INPUT_FRAMES runs usando k-way merge.
 * Usa (B-1)=4 buffers de entrada e 1 buffer de saída.
 *
 * @param runs    Grupo de runs a intercalar
 * @param col_idx Índice da coluna de ordenação
 * @return        Uma única run ordenada resultante
 */
Run mergeRunGroup(const std::vector<Run*>& runs, int col_idx) {
    int k = (int)runs.size();

    // Posição atual em cada run: (índice_de_página, índice_de_tupla)
    std::vector<int> page_idx(k, 0);
    std::vector<int> tuple_idx(k, 0);

    // Buffer de saída (1 frame)
    Page output_buffer;
    Run merged_run;

    // K-way merge: a cada passo extrai a tupla de menor chave
    while (true) {
        int min_run = -1;
        std::string min_key;

        // Encontra a run com a menor chave atual
        for (int i = 0; i < k; i++) {
            int pi = page_idx[i];
            int ti = tuple_idx[i];

            if (pi >= (int)runs[i]->size()) continue; // run esgotada
            if (ti >= (*runs[i])[pi].num_tuples) continue;

            std::string key = (*runs[i])[pi].tuples[ti].getCol(col_idx);
            if (min_run == -1 || key < min_key) {
                min_key = key;
                min_run = i;
            }
        }

        if (min_run == -1) break; // todas as runs esgotadas

        // Extrai a tupla da run vencedora
        int pi = page_idx[min_run];
        int ti = tuple_idx[min_run];
        Tuple t = (*runs[min_run])[pi].tuples[ti];

        // Avança o ponteiro na run vencedora
        tuple_idx[min_run]++;
        if (tuple_idx[min_run] >= (*runs[min_run])[page_idx[min_run]].num_tuples) {
            page_idx[min_run]++;
            tuple_idx[min_run] = 0;
        }

        // Adiciona ao buffer de saída
        if (output_buffer.isFull()) {
            // Buffer cheio: grava no disco simulado
            merged_run.push_back(output_buffer);
            output_buffer.clear();
        }
        output_buffer.addTuple(t);
    }

    // Descarrega o buffer de saída restante
    if (!output_buffer.isEmpty())
        merged_run.push_back(output_buffer);

    return merged_run;
}

/**
 * Fase 2 do External Sort: executa passagens de intercalação até
 * restar apenas uma run ordenada.
 *
 * @param runs    Runs iniciais (modificado in-place)
 * @param col_idx Índice da coluna de ordenação
 */
void mergePhase(std::vector<Run>& runs, int col_idx) {
    int pass = 1;

    while ((int)runs.size() > 1) {
        std::cout << "  [Fase 2] Passagem " << pass
                  << " de intercalação: " << runs.size() << " runs" << std::endl;

        std::vector<Run> new_runs;

        // Processa grupos de até MAX_INPUT_FRAMES runs
        for (int r = 0; r < (int)runs.size(); r += MAX_INPUT_FRAMES) {
            int end = std::min(r + MAX_INPUT_FRAMES, (int)runs.size());

            // Monta o grupo de ponteiros para as runs
            std::vector<Run*> group;
            for (int i = r; i < end; i++)
                group.push_back(&runs[i]);

            Run merged = mergeRunGroup(group, col_idx);
            new_runs.push_back(merged);

            std::cout << "    Intercalação de " << group.size()
                      << " runs -> " << merged.size()
                      << " páginas" << std::endl;
        }

        runs = new_runs;
        pass++;
    }
}

/**
 * Executa a ordenação externa completa de uma tabela.
 * Não modifica a tabela original.
 *
 * @param table       Tabela de entrada
 * @param join_col    Nome da coluna de junção
 * @param table_name  Nome para mensagens de log
 * @return            Run com as tuplas da tabela ordenadas
 */
Run externalSort(const Table& table, const std::string& join_col,
                 const std::string& table_name) {
    int col_idx = table.schema.getIndex(join_col);
    if (col_idx == -1) {
        std::cerr << "[ERRO] Coluna '" << join_col
                  << "' não encontrada na tabela '" << table_name << "'" << std::endl;
        return {};
    }

    std::cout << "\n[External Sort] Tabela: '" << table_name
              << "', coluna: '" << join_col << "'" << std::endl;
    std::cout << "  " << table.num_pages() << " páginas, "
              << table.totalTuples() << " tuplas" << std::endl;

    // Fase 1: gera runs iniciais
    auto runs = createInitialRuns(table, col_idx, table_name);

    // Fase 2: intercalação externa
    if ((int)runs.size() > 1)
        mergePhase(runs, col_idx);
    else
        std::cout << "  [Fase 2] Apenas 1 run, sem necessidade de intercalação." << std::endl;

    std::cout << "  Ordenação concluída: " << runs[0].size()
              << " páginas no disco simulado." << std::endl;

    return runs[0]; // única run ordenada
}

// ============================================================
// Merge Join
// ============================================================

/**
 * Combina os esquemas das duas tabelas para o resultado da junção.
 * Prefixa colunas da segunda tabela com "w_" em caso de conflito de nomes.
 */
Schema combineSchemas(const Schema& s1, const Schema& s2) {
    Schema result;
    for (const auto& name : s1.col_names)
        result.addCol(name);
    for (const auto& name : s2.col_names) {
        std::string col_name = (result.getIndex(name) != -1) ? ("w_" + name) : name;
        result.addCol(col_name);
    }
    return result;
}

/**
 * Concatena duas tuplas em uma só.
 */
Tuple joinTuples(const Tuple& t1, const Tuple& t2) {
    Tuple result;
    result.cols = t1.cols;
    for (const auto& col : t2.cols)
        result.cols.push_back(col);
    return result;
}

/**
 * Executa a etapa de Merge Join nas tabelas já ordenadas.
 * Produz todas as combinações de tuplas com chave igual (produto cartesiano no grupo).
 * Utiliza buffer de saída (1 frame = 1 página).
 *
 * @param sorted_grapes  Run ordenada de Grapes
 * @param grapes_col_idx Índice da coluna de junção em Grapes
 * @param sorted_wines   Run ordenada de Wines
 * @param wines_col_idx  Índice da coluna de junção em Wines
 * @param result_schema  Esquema da tabela resultado
 * @return               Tabela resultado da junção
 */
Table mergeJoin(const Run& sorted_grapes, int grapes_col_idx,
                const Run& sorted_wines,  int wines_col_idx,
                const Schema& result_schema) {
    std::cout << "\n[Merge Join] Iniciando fase de comparação..." << std::endl;

    // Extrai todas as tuplas das runs ordenadas
    std::vector<Tuple> G = runToTuples(sorted_grapes);
    std::vector<Tuple> W = runToTuples(sorted_wines);

    // Tabela resultado
    Table result;
    result.schema = result_schema;

    // Buffer de saída (1 frame)
    Page output_buffer;
    int total_matches = 0;

    int gi = 0, wi = 0;

    // Percorre sequencialmente as duas tabelas ordenadas
    while (gi < (int)G.size() && wi < (int)W.size()) {
        const std::string& gk = G[gi].getCol(grapes_col_idx);
        const std::string& wk = W[wi].getCol(wines_col_idx);

        if (gk < wk) {
            gi++; // avança em Grapes
        } else if (gk > wk) {
            wi++; // avança em Wines
        } else {
            // Grupo de chaves iguais: encontra os limites do grupo em cada tabela
            int gi_end = gi, wi_end = wi;
            while (gi_end < (int)G.size() && G[gi_end].getCol(grapes_col_idx) == gk)
                gi_end++;
            while (wi_end < (int)W.size() && W[wi_end].getCol(wines_col_idx) == gk)
                wi_end++;

            // Produz todas as combinações do produto cartesiano do grupo
            for (int ii = gi; ii < gi_end; ii++) {
                for (int jj = wi; jj < wi_end; jj++) {
                    Tuple joined = joinTuples(G[ii], W[jj]);

                    // Gerencia o buffer de saída
                    if (output_buffer.isFull()) {
                        // Grava página cheia no disco simulado (tabela resultado)
                        result.pages.push_back(output_buffer);
                        output_buffer.clear();
                    }
                    output_buffer.addTuple(joined);
                    total_matches++;
                }
            }

            // Avança ambos os ponteiros para além do grupo
            gi = gi_end;
            wi = wi_end;
        }
    }

    // Descarrega buffer de saída restante
    if (!output_buffer.isEmpty())
        result.pages.push_back(output_buffer);

    std::cout << "[Merge Join] Concluído: " << total_matches
              << " tuplas resultado em " << result.num_pages()
              << " páginas." << std::endl;

    return result;
}

// ============================================================
// Sort-Merge Join: orquestra tudo
// ============================================================

/**
 * Executa o operador Sort-Merge Join completo entre Grapes e Wines.
 * As tabelas originais não são modificadas.
 *
 * @param grapes          Tabela Grapes
 * @param grapes_join_col Coluna de junção em Grapes ("chave_primaria")
 * @param wines           Tabela Wines
 * @param wines_join_col  Coluna de junção em Wines ("chave_estrangeira")
 * @return                Tabela resultado da junção
 */
Table sortMergeJoin(const Table& grapes, const std::string& grapes_join_col,
                    const Table& wines,  const std::string& wines_join_col) {
    std::cout << "\n==============================" << std::endl;
    std::cout << " Sort-Merge Join" << std::endl;
    std::cout << " Grapes." << grapes_join_col
              << " = Wines." << wines_join_col << std::endl;
    std::cout << "==============================" << std::endl;

    int grapes_col_idx = grapes.schema.getIndex(grapes_join_col);
    int wines_col_idx  = wines.schema.getIndex(wines_join_col);

    if (grapes_col_idx == -1) {
        std::cerr << "[ERRO] Coluna '" << grapes_join_col << "' não encontrada em Grapes." << std::endl;
        return {};
    }
    if (wines_col_idx == -1) {
        std::cerr << "[ERRO] Coluna '" << wines_join_col << "' não encontrada em Wines." << std::endl;
        return {};
    }

    // Etapa 1: Ordenação externa das duas tabelas
    Run sorted_grapes = externalSort(grapes, grapes_join_col, "Grapes");
    Run sorted_wines  = externalSort(wines,  wines_join_col,  "Wines");

    // Etapa 2: Merge Join
    Schema result_schema = combineSchemas(grapes.schema, wines.schema);
    Table result = mergeJoin(sorted_grapes, grapes_col_idx,
                             sorted_wines,  wines_col_idx,
                             result_schema);

    return result;
}

// ============================================================
// Exibição de resultados
// ============================================================

/**
 * Imprime as colunas e tuplas de uma tabela.
 * @param max_rows  Limite de linhas a exibir (-1 = todas)
 */
void printTable(const Table& table, int max_rows = -1) {
    // Cabeçalho
    const int COL_WIDTH = 25;
    for (const auto& name : table.schema.col_names) {
        std::string s = name.substr(0, COL_WIDTH - 1);
        std::cout << "| " << s;
        std::cout << std::string(COL_WIDTH - (int)s.size(), ' ');
    }
    std::cout << "|" << std::endl;
    std::cout << std::string(table.schema.num_cols * (COL_WIDTH + 2), '-') << std::endl;

    int row = 0;
    for (const auto& page : table.pages) {
        for (int i = 0; i < page.num_tuples; i++) {
            if (max_rows > 0 && row >= max_rows) {
                std::cout << "... (mostrando " << max_rows
                          << " de " << table.totalTuples() << " tuplas)" << std::endl;
                return;
            }
            for (int j = 0; j < (int)page.tuples[i].cols.size(); j++) {
                std::string s = page.tuples[i].cols[j];
                if ((int)s.size() > COL_WIDTH - 1) s = s.substr(0, COL_WIDTH - 4) + "...";
                std::cout << "| " << s;
                std::cout << std::string(COL_WIDTH - (int)s.size(), ' ');
            }
            std::cout << "|" << std::endl;
            row++;
        }
    }
}

// ============================================================
// Casos de teste
// ============================================================

/**
 * Verifica que a tabela resultado está corretamente ordenada pela
 * coluna de junção de Grapes.
 */
bool testResultSorted(const Table& result) {
    int col = result.schema.getIndex("chave_primaria");
    if (col == -1) return false;

    std::string prev = "";
    for (const auto& page : result.pages) {
        for (int i = 0; i < page.num_tuples; i++) {
            std::string key = page.tuples[i].getCol(col);
            if (key < prev) return false;
            prev = key;
        }
    }
    return true;
}

/**
 * Verifica que os valores da coluna de junção são iguais em ambos os lados
 * do resultado.
 */
bool testJoinKeysMatch(const Table& result) {
    int grape_col = result.schema.getIndex("chave_primaria");
    int wine_col  = result.schema.getIndex("chave_estrangeira");
    if (grape_col == -1 || wine_col == -1) return false;

    for (const auto& page : result.pages)
        for (int i = 0; i < page.num_tuples; i++)
            if (page.tuples[i].getCol(grape_col) != page.tuples[i].getCol(wine_col))
                return false;
    return true;
}

/**
 * Verifica que o número de páginas é consistente com o número de tuplas.
 */
bool testPageConsistency(const Table& result) {
    for (const auto& page : result.pages)
        if (page.num_tuples < 1 || page.num_tuples > TUPLES_PER_PAGE)
            return false;
    return true;
}

void runTests(const Table& result) {
    std::cout << "\n=== Testes de Validação ===" << std::endl;

    bool t1 = testJoinKeysMatch(result);
    std::cout << "[TESTE 1] Chaves de junção iguais em todas as tuplas: "
              << (t1 ? "PASSOU" : "FALHOU") << std::endl;

    bool t2 = testPageConsistency(result);
    std::cout << "[TESTE 2] Consistência de páginas (1-12 tuplas/página): "
              << (t2 ? "PASSOU" : "FALHOU") << std::endl;

    bool t3 = result.totalTuples() > 0;
    std::cout << "[TESTE 3] Resultado não vazio: "
              << (t3 ? "PASSOU" : "FALHOU") << std::endl;

    std::cout << "[TESTE 4] Número de tuplas no resultado: "
              << result.totalTuples() << std::endl;
    std::cout << "[TESTE 5] Número de páginas no resultado: "
              << result.num_pages() << std::endl;
}

// ============================================================
// Escrita do resultado em arquivo
// ============================================================

/**
 * Grava o resultado completo da junção em um arquivo .txt.
 * Cria o arquivo se não existir. Se já existir, sobrescreve.
 * Retorna true em caso de sucesso, false em caso de erro.
 */
bool writeResultToFile(const Table& table, const std::string& filename) {
    std::ofstream file(filename);

    if (!file.is_open()) {
        std::cerr << "[ERRO] Não foi possível criar o arquivo: " << filename << std::endl;
        return false;
    }

    const int COL_WIDTH = 25;
    const int total_width = table.schema.num_cols * (COL_WIDTH + 2);

    // Cabeçalho
    file << "Sort-Merge Join - Resultado Completo" << std::endl;
    file << "Tuplas: " << table.totalTuples()
         << " | Páginas: " << table.num_pages() << std::endl;
    file << std::string(total_width, '=') << std::endl;

    for (const auto& name : table.schema.col_names) {
        std::string s = name.substr(0, COL_WIDTH - 1);
        file << "| " << s << std::string(COL_WIDTH - (int)s.size(), ' ');
    }
    file << "|" << std::endl;
    file << std::string(total_width, '-') << std::endl;

    // Tuplas, página por página
    int page_num = 1;
    for (const auto& page : table.pages) {
        for (int i = 0; i < page.num_tuples; i++) {
            for (int j = 0; j < (int)page.tuples[i].cols.size(); j++) {
                std::string s = page.tuples[i].cols[j];
                if ((int)s.size() > COL_WIDTH - 1)
                    s = s.substr(0, COL_WIDTH - 4) + "...";
                file << "| " << s << std::string(COL_WIDTH - (int)s.size(), ' ');
            }
            file << "|" << std::endl;
        }
        // Separador entre páginas
        file << std::string(total_width, '-') << " pag." << page_num++ << std::endl;
    }

    file.close();
    return true;
}

// ============================================================
// Main
// ============================================================

int main(int argc, char* argv[]) {
    std::cout << "=================================================" << std::endl;
    std::cout << " Sistemas de Bancos de Dados - 2026-1       " << std::endl;
    std::cout << " Trabalho III - Junção Sort-Merge                " << std::endl;
    std::cout << "=================================================" << std::endl;
    std::cout << "Buffer: B=" << BUFFER_FRAMES << " frames | "
              << TUPLES_PER_PAGE << " tuplas/página" << std::endl << std::endl;

    // Caminhos dos arquivos (podem ser passados como argumento)
    std::string grapes_path = (argc > 1) ? argv[1] : "data/grapes.csv";
    std::string wines_path  = (argc > 2) ? argv[2] : "data/wines.csv";

    // --- Carregamento das tabelas ---
    std::cout << "[Leitura] Carregando " << grapes_path << "..." << std::endl;
    Table grapes = loadCSV(grapes_path);
    if (grapes.num_pages() == 0) {
        std::cerr << "[ERRO] Grapes vazia ou não encontrada." << std::endl;
        return 1;
    }
    std::cout << "[Leitura] Grapes: " << grapes.num_pages() << " páginas, "
              << grapes.totalTuples() << " tuplas" << std::endl;

    std::cout << "[Leitura] Carregando " << wines_path << "..." << std::endl;
    Table wines = loadCSV(wines_path);
    if (wines.num_pages() == 0) {
        std::cerr << "[ERRO] Wines vazia ou não encontrada." << std::endl;
        return 1;
    }
    std::cout << "[Leitura] Wines: " << wines.num_pages() << " páginas, "
              << wines.totalTuples() << " tuplas" << std::endl;

    // --- Sort-Merge Join ---
    Table result = sortMergeJoin(
        grapes, "chave_primaria",
        wines,  "chave_estrangeira"
    );

    // --- Testes de validação ---
    runTests(result);

    // --- Grava resultado completo em arquivo ---
    std::string output_file = "resultado_join.txt";
    std::cout << "\n[Saída] Gravando resultado completo em '" << output_file << "'..." << std::endl;
    if (writeResultToFile(result, output_file))
        std::cout << "[Saída] Arquivo gravado com sucesso." << std::endl;

    // --- Exibição dos resultados ---
    std::cout << "\n=== Resultado da Junção (primeiras 20 tuplas) ===" << std::endl;
    printTable(result, 20);

    std::cout << "\n=== Resumo Final ===" << std::endl;
    std::cout << "Tuplas em Grapes : " << grapes.totalTuples() << std::endl;
    std::cout << "Tuplas em Wines  : " << wines.totalTuples()  << std::endl;
    std::cout << "Tuplas resultado : " << result.totalTuples() << std::endl;
    std::cout << "Páginas resultado: " << result.num_pages()   << std::endl;

    return 0;
}