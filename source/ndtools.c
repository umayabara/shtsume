//
//  ndtools.c
//  shtsume
//
//  Created by Hkijin on 2022/02/03.
//

#include <stdlib.h>
#include <errno.h>
#include "ndtools.h"

/* ---------------
   スタティック変数
 --------------- */
static bool st_disp_flag;            /* tsume_debugの終了flag */

/* ---------------
   スタティック関数
 --------------- */
static void _tsume_print_or     (const sdata_t  *sdata,
                                 tbase_t        *tbase,
                                 unsigned int    flag );
static void _tsume_print_and    (const sdata_t  *sdata,
                                 tbase_t        *tbase,
                                 unsigned int    flag );
static int _tsume_fprint_or     (FILE           *stream,
                                 const sdata_t  *sdata,
                                 tbase_t        *tbase,
                                 unsigned int    flag );
static int _tsume_fprint_and    (FILE           *stream,
                                 const sdata_t  *sdata,
                                 tbase_t        *tbase,
                                 unsigned int    flag );
static void tsume_debug_or      (const sdata_t   *sdata,
                                 tbase_t        *tbase);
static void tsume_debug_and     (const sdata_t   *sdata,
                                 tbase_t        *tbase);
static void gen_kif_file_or     (FILE *restrict  stream,
                                 const sdata_t   *sdata,
                                 tbase_t         *tbase,
                                 move_t           prev );
static void gen_kif_file_and    (FILE *restrict  stream,
                                 const sdata_t   *sdata,
                                 tbase_t         *tbase,
                                 move_t           prev );

/*
 * 攻方候補の排他性(exclusivity)判定。
 * ある王手可能局面(OR節点)で実際に選択された着手が、詰みに至る唯一の
 * 候補手であるかどうかを表す。
 *   exclusive    : 他の合法な王手候補は全て不詰と証明済み
 *   nonexclusive : 他の合法な王手候補に、詰みと証明された手が存在する
 *   unverified   : 詰みと証明された他候補は無いが、未解決(pn!=0 かつ dn!=0)
 *                  な候補が残っている(証明木不在＝不詰、と混同しない)
 *
 * 注意: この判定は着手の種別(盤上の移動か持駒打かなど)や打つ駒の種類
 * (歩・香・桂・銀・金・角・飛・と金等)に一切依存しない。判定基準は
 * 純粋に「詰みへの到達が証明されているか」のみであり、大駒(飛・角、
 * またはその成駒)を打つ手を特別扱いしたり、駒の大小で優先順位や
 * スコアを変えたりすることはない。合法な王手候補は generate_check が
 * 返す全候補を等しく扱い、選択手との比較も raw な prev_pos/new_pos の
 * 一致判定のみで行う(json_select_move と同じ方式)。
 *
 * 重要: ここでいう「排他性(exclusivity)」は汎用的な概念であり、将棋の
 * 「限定打」とは別概念である。「限定打」は別途定義・実装される
 * (どのような基準で判定するかも含め未確定)。ソルバーは
 * exclusive/nonexclusive/unverified という汎用的な排他性のみを
 * 全ての攻方候補について報告し、「限定打」に該当するか否かの判断・
 * ラベル付け・推論は一切行わない。exclusive かつ打ち駒であっても、
 * それだけでは「限定打」を意味しない。駒種による絞り込みは元より、
 * 汎用排他性から「限定打」を推論する処理そのものをソルバー側に
 * 実装してはならない。
 */
typedef enum {
    JSON_EXCLUSIVITY_EXCLUSIVE,
    JSON_EXCLUSIVITY_NONEXCLUSIVE,
    JSON_EXCLUSIVITY_UNVERIFIED
} json_exclusivity_status_t;

typedef enum {
    JSON_BOUNDED_NO_MATE,
    JSON_BOUNDED_MATE,
    JSON_BOUNDED_ABORTED
} json_bounded_mate_status_t;

/* 一局面での王手候補数は将棋の合法手数の範囲で十分な余裕を持たせる */
#define JSON_EXCLUSIVITY_MAX_ALT   200

/* 一般手の追加DFPN探索に許容する探索高の余裕。 */
#define JSON_EXCLUSIVITY_DEPTH_MARGIN   20
/*
 * 線駒打ちの代案は固定深さ探索に加え、小さい証明数閾値でも調べる。
 * これにより同手数の別詰みを拾いつつ、不詰候補の完全反証で停止しない。
 */
#define JSON_EXCLUSIVITY_PROOF_PN_LIMIT 16

/* 代案の不詰反証線は、循環しない異常な木でも有限時間で打ち切る。 */
#define JSON_NO_MATE_LINE_MAX_PLIES 256

typedef struct {
    bool complete;
    const char *terminal_reason;
} json_no_mate_line_result_t;

typedef struct {
    zkey_t zkey;
    mkey_t sente_hand;
    mkey_t gote_hand;
    turn_t turn;
} json_position_key_t;

/*
 * 線駒打ちの手数では comparison_scope が限定打の比較に切り替わり、
 * alternative_details が同駒種・同半直線の打だけになる。紛れの候補には
 * それ以外の王手も要るので、限定打の判定はそのままに、全王手を対象にした
 * 解析を general_alternatives へ別に持つ。
 */
typedef struct {
    move_t       move;
    tdata_t      result;
    bool         horizon_out;
    char        *refutation_line;
    bool         refutation_complete;
    const char  *refutation_reason;
} json_general_alternative_t;

typedef struct json_exclusivity_entry {
    unsigned int ply;                 /* 攻方着手の手数(1始まり)        */
    move_t       move;                /* 実際に選択された着手           */
    json_exclusivity_status_t status;
    bool         same_piece_drop_scope;
    unsigned int alternative_count;   /* 選択手以外の合法な王手候補数    */
    unsigned int selected_mate_moves; /* 選択手から詰みまでの手数         */
    unsigned int comparison_search_plies;
    bool has_unresolved_alternatives; /* 追加探索後も未解決の候補があるか */
    move_t       alternatives[JSON_EXCLUSIVITY_MAX_ALT];
    tdata_t      alternative_results[JSON_EXCLUSIVITY_MAX_ALT];
    bool         alternative_bounded_out[JSON_EXCLUSIVITY_MAX_ALT];
    /* 初手からの固定手数(探索地平)内に詰みが無いと確認できた候補 */
    bool         alternative_horizon_out[JSON_EXCLUSIVITY_MAX_ALT];
    char        *alternative_refutation_lines[JSON_EXCLUSIVITY_MAX_ALT];
    bool         alternative_refutation_complete[JSON_EXCLUSIVITY_MAX_ALT];
    const char  *alternative_refutation_reasons[JSON_EXCLUSIVITY_MAX_ALT];
    move_t       proven_mating[JSON_EXCLUSIVITY_MAX_ALT];
    unsigned int proven_mating_moves[JSON_EXCLUSIVITY_MAX_ALT];
    char        *proven_mating_lines[JSON_EXCLUSIVITY_MAX_ALT];
    unsigned int proven_mating_count; /* 詰みと証明された他候補の数     */
    json_general_alternative_t
                 general_alternatives[JSON_EXCLUSIVITY_MAX_ALT];
    unsigned int general_count;       /* 限定打の手数での全王手の解析数  */
    struct json_exclusivity_entry *next;
} json_exclusivity_entry_t;

typedef struct {
    bool complete;
    bool mate;
    unsigned int terminal_ply;
    unsigned int attacker_hand_count;
    json_exclusivity_entry_t *exclusivity; /* 手順中の各攻方着手の排他性 */
    bool exclusivity_complete;
} json_line_result_t;

typedef struct json_baseline_line {
    unsigned int branch_ply;
    move_t defender_move;
    FILE *stream;
    json_line_result_t result;
    bool first_move;
    struct json_baseline_line *next;
} json_baseline_line_t;

static json_baseline_line_t *st_json_baselines;
static bool st_json_capture_baselines;
/* 各分岐から数えた有界不詰探索の手数。0 なら判定しない。 */
static unsigned int st_json_bounded_plies;
/*
 * 有界不詰探索の上限。手数を伸ばすと探索量が急に増えるため、上限に達したら
 * 打ち切って、その候補は unresolved のまま残す。
 *   node_budget  : ノード数の上限(0で無制限)。再現性が要るときに使う
 *   deadline     : 1回の解析全体に許す時間の締め切り
 * どちらも限定打側の固定深さ探索には適用しない(そちらは既に校正済みのため)。
 */
static uint64_t st_json_bounded_node_budget;
static uint64_t st_json_bounded_nodes;
static double   st_json_bounded_seconds;
static clock_t  st_json_bounded_deadline;
static bool     st_json_bounded_deadline_set;
static bool     st_json_bounded_active;
static tbase_t *st_json_exclusivity_tbase;
static json_exclusivity_entry_t *st_json_principal_exclusivity;

static mvlist_t* json_prepare_or(const sdata_t *sdata, tbase_t *tbase);
static mvlist_t* json_optimize_or(
    mvlist_t *list, const sdata_t *sdata, tbase_t *tbase);
static mvlist_t* json_prepare_and(const sdata_t *sdata, tbase_t *tbase);
static json_bounded_mate_status_t json_bounded_mate_or(
    const sdata_t *sdata, unsigned int remaining, tbase_t *tbase);
static json_bounded_mate_status_t json_bounded_mate_and(
    const sdata_t *sdata, unsigned int remaining, tbase_t *tbase);
static json_line_result_t json_emit_line_or(
    FILE *stream, const sdata_t *sdata, tbase_t *tbase, bool *first_move,
    bool research_lines, bool analyze_exclusivity);
static json_line_result_t json_emit_line_and(
    FILE *stream, const sdata_t *sdata, tbase_t *tbase, bool *first_move,
    bool research_lines, bool analyze_exclusivity);
static json_line_result_t json_emit_optimal_or(
    FILE *stream, const sdata_t *sdata, tbase_t *tbase, bool *first_move,
    unsigned int remaining, bool analyze_exclusivity);
static json_line_result_t json_emit_optimal_and(
    FILE *stream, const sdata_t *sdata, tbase_t *tbase, bool *first_move,
    unsigned int remaining, bool analyze_exclusivity);
static bool json_collect_variations_or(
    FILE *stream, const sdata_t *sdata, tbase_t *tbase, bool *first_variation,
    const move_t *principal, unsigned int principal_length,
    bool research_lines, bool analyze_exclusivity, bool *principal_valid);
static bool json_collect_variations_and(
    FILE *stream, const sdata_t *sdata, tbase_t *tbase, bool *first_variation,
    const move_t *principal, unsigned int principal_length,
    bool research_lines, bool analyze_exclusivity, bool *principal_valid);
static void json_emit_move(
    FILE *stream, move_t move, bool *first_move);
static char* json_read_stream(FILE *stream);
static json_no_mate_line_result_t json_emit_no_mate_line(
    FILE *stream, const sdata_t *sdata, tbase_t *tbase);
static mvlist_t* json_select_move(mvlist_t *list, move_t move);
static void json_free_baselines(json_baseline_line_t *list);
static json_exclusivity_entry_t* json_analyze_or_exclusivity(
    const sdata_t *sdata, tbase_t *tbase, mvlist_t *list,
    move_t selected_move, unsigned int ply);
static void json_free_exclusivity(json_exclusivity_entry_t *list);
static void json_print_exclusivity(
    FILE *stream, json_exclusivity_entry_t *list);

/*
 * 探索情報の表示
 * 探索深さ、着手、証明数、反証数、
 */
int   search_inf_sprintf (char *restrict str,
                          const mvlist_t *mvlist,
                          const sdata_t *sdata )
{
    int num = 0;
    num += sprintf(str+num, " %d ", S_COUNT(sdata));
    num += sprintf(str+num, " 0X%llX ", S_ZKEY(sdata));
    num += move_sprintf(str+num, mvlist->mlist->move, sdata);
    num += sprintf(str+num, "(%u %u)\n", mvlist->tdata.pn, mvlist->tdata.dn);
    return num;
}
int   search_inf_fprintf (FILE *restrict stream,
                          const mvlist_t *mvlist,
                          const sdata_t *sdata )
{
    int num = 0;
    num += fprintf(stream, " %d ", S_COUNT(sdata));
    num += fprintf(stream, " 0X%llX ", S_ZKEY(sdata));
    num += move_fprintf(stream, mvlist->mlist->move, sdata);
    num += fprintf(stream, "(%u %u)\n", mvlist->tdata.pn, mvlist->tdata.dn);
    return num;
}

/*
 * mlistの内容表示
 */
int mlist_sprintf   (char *restrict str,
                     const mlist_t *mlist,
                     const sdata_t *sdata )
{
    int num = 0;
    const mlist_t *list = mlist;
    while (list) {
        num += move_sprintf(str+num, list->move, sdata);
        num += sprintf(str+num, " ");
        list = list->next;
    }
    num += sprintf(str+num, "\n");
    return num;
}

int mlist_fprintf   (FILE *restrict stream,
                     const mlist_t *mlist,
                     const sdata_t *sdata )
{
    int num = 0;
    const mlist_t *list = mlist;
    while (list) {
        num += move_fprintf(stream, list->move, sdata);
        num += fprintf(stream, " ");
        list = list->next;
    }
    num += fprintf(stream, "\n");
    return num;
}

/*
 * mvlistの着手表示
 */
int mvlist_sprintf  (char *restrict str,
                     const mvlist_t *mvlist,
                     const sdata_t *sdata )
{
    int num = 0;
    const mvlist_t *list = mvlist;
    while (list) {
        num += mlist_sprintf(str+num, list->mlist, sdata);
        list = list->next;
    }
    return num;
}

int mvlist_fprintf  (FILE *restrict stream,
                     const mvlist_t *mvlist,
                     const sdata_t *sdata )
{
    int num = 0;
    const mvlist_t *list = mvlist;
    while (list) {
        num += mlist_fprintf(stream, list->mlist, sdata);
        list = list->next;
    }
    return num;
}

/*
 * mvlistの内容表示
 */

int mvlist_sprintf_with_item (char *restrict str,
                              const mvlist_t *mvlist,
                              const sdata_t *sdata )
{
    int num = 0;
    int item = 0;
    num += sprintf(str+num,
                   "id 着手(*) pn,dn,sh,hinc,inc,shh,cu,pr,len,nou,nou2\n");
    const mvlist_t *list = mvlist;
    while (list) {
        item++;
        if (list->mlist) {
            num += sprintf(str+num,"%d ", item);
            num += move_sprintf(str+num, list->mlist->move, sdata);
            if (list->mlist->next) {
                num += sprintf(str+num, "*");
            }
            num += sprintf(str+num,
                           " %d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n",
                           list->tdata.pn,
                           list->tdata.dn,
                           list->tdata.sh,
                           list->hinc,
                           list->inc,
                           list->search,
                           list->cu,
                           list->pr,
                           list->length,
                           list->nouse,
                           list->nouse2
                           );
        }
        list = list->next;
    }
    return num;
}

int mvlist_fprintf_with_item (FILE *restrict stream,
                              const mvlist_t *mvlist,
                              const sdata_t *sdata )
{
    int num = 0;
    int item = 0;
    num += fprintf(stream,
                   "id 着手(*) pn,dn,sh,hinc,inc,shh,cu,pr,len,nou,nou2\n");
    const mvlist_t *list = mvlist;
    while (list) {
        item++;
        if (list->mlist) {
            num += fprintf(stream,"%d ", item);
            num += move_fprintf(stream, list->mlist->move, sdata);
            if (list->mlist->next) {
                num += fprintf(stream, "*");
            }
            num += fprintf(stream,
                           " %d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n",
                           list->tdata.pn,
                           list->tdata.dn,
                           list->tdata.sh,
                           list->hinc,
                           list->inc,
                           list->search,
                           list->cu,
                           list->pr,
                           list->length,
                           list->nouse,
                           list->nouse2
                           );
        }
        list = list->next;
    }
    return num;
}

void tsume_print                (const sdata_t   *sdata,
                                 tbase_t         *tbase,
                                 unsigned int      flag)
{
    if(g_redundant){
        printf("参考詰手順　\n"
               "手数:　着手(先頭が選択着手） \n");
    } else{
        printf("詰手順　\n"
               "手数:　着手（ PN値 残り手数 駒余り 余り駒数 )\n");
    }
    _tsume_print_or(sdata, tbase, flag);
    return;
}

int tsume_fprint                (FILE            *stream,
                                 const sdata_t   *sdata,
                                 tbase_t         *tbase,
                                 unsigned int     flag )
{
    int num = 0;
    
    if(g_redundant){
        num += fprintf(stream, "参考詰手順　\n"
                               "手数:　着手(先頭が選択着手） \n");
    } else{
        num += fprintf(stream, "詰手順　\n"
                               "手数:　着手（ PN値 残り手数 駒余り 余り駒数 )\n");
    }
    
    num += _tsume_fprint_or(stream, sdata, tbase, flag);
    return num;
}

void tsume_json_set_bounded_no_mate(unsigned int branch_plies,
                                    uint64_t     node_budget,
                                    double       seconds)
{
    st_json_bounded_plies = branch_plies;
    st_json_bounded_node_budget = node_budget;
    st_json_bounded_seconds = seconds;
    st_json_bounded_deadline_set = false;
    st_json_bounded_active = false;
    return;
}

bool tsume_json_variations_fprint(
    FILE *stream, const sdata_t *sdata, tbase_t *tbase,
    const move_t *principal, unsigned int principal_length,
    bool research_lines, bool analyze_exclusivity,
    bool *principal_valid)
{
    bool first_variation = true;
    *principal_valid = true;
    st_json_baselines = NULL;
    st_json_principal_exclusivity = NULL;
    st_json_exclusivity_tbase = analyze_exclusivity
        ? create_tbase(
            MIN(tbase->sz_elm, 64 * MCARDS_PER_MBYTE - 1))
        : NULL;
    if(research_lines){
        FILE *baseline_output = tmpfile();
        if(baseline_output){
            bool baseline_first = true;
            bool baseline_principal_valid = true;
            st_json_capture_baselines = true;
            json_collect_variations_or(
                baseline_output, sdata, tbase, &baseline_first,
                principal, principal_length, false, false,
                &baseline_principal_valid);
            st_json_capture_baselines = false;
            fclose(baseline_output);
        }
    }
    fprintf(stream, "[");
    bool complete = json_collect_variations_or(
        stream, sdata, tbase, &first_variation, principal, principal_length,
        research_lines, analyze_exclusivity, principal_valid);
    fprintf(stream, "]");
    if(analyze_exclusivity){
        fprintf(stream, ",\"principal_attacker_move_exclusivity\":");
        json_print_exclusivity(stream, st_json_principal_exclusivity);
        json_free_exclusivity(st_json_principal_exclusivity);
        st_json_principal_exclusivity = NULL;
    }
    json_free_baselines(st_json_baselines);
    st_json_baselines = NULL;
    if(st_json_exclusivity_tbase){
        destroy_tbase(st_json_exclusivity_tbase);
        st_json_exclusivity_tbase = NULL;
    }
    return complete;
}

static mvlist_t* json_prepare_or(const sdata_t *sdata, tbase_t *tbase)
{
    MAKE_TREE_SET_PROTECT(sdata, S_TURN(sdata), tbase);
    mvlist_t *list = generate_check(sdata, tbase);
    if(!list) return NULL;

    sdata_t sbuf;
    mvlist_t *tmp = list;
    while(tmp){
        memcpy(&sbuf, sdata, sizeof(sdata_t));
        sdata_key_forward(&sbuf, tmp->mlist->move);
        make_tree_lookup(&sbuf, tmp, S_TURN(sdata), tbase);
        tmp->length =
            g_distance[ENEMY_OU(sdata)][NEW_POS(tmp->mlist->move)];
        if(tmp->length > 1 &&
           tmp->tdata.pn == 1 &&
           tmp->tdata.dn == 1 &&
           tmp->tdata.sh == 0 &&
           MV_DROP(tmp->mlist->move)) tmp->tdata.pn = tmp->length;
        tmp = tmp->next;
    }
    return sdata_mvlist_sort(list, sdata, proof_number_comp);
}

static mvlist_t* json_optimize_or(
    mvlist_t *list, const sdata_t *sdata, tbase_t *tbase)
{
    mvlist_t *best = list;
    while(best && best->tdata.pn) best = best->next;
    if(!best) return list;

    mvlist_t *candidate = list;
    while(candidate){
        if(candidate != best){
            tdata_t threshold = {
                INFINATE-1, INFINATE-1, best->tdata.sh + 1
            };
            sdata_t child;
            memcpy(&child, sdata, sizeof(sdata_t));
            sdata_move_forward(&child, candidate->mlist->move);
            bns_and(&child, &threshold, candidate, tbase);
            if(!candidate->tdata.pn &&
               researched_line_comp(candidate, best, sdata) < 0){
                best = candidate;
            }
        }
        candidate = candidate->next;
    }
    return sdata_mvlist_sort(list, sdata, researched_line_comp);
}

static mvlist_t* json_prepare_and(const sdata_t *sdata, tbase_t *tbase)
{
    mvlist_t *list = generate_evasion(sdata, tbase);
    if(!list) return NULL;

    turn_t tn = TURN_FLIP(S_TURN(sdata));
    sdata_t sbuf;
    mvlist_t *tmp = list;
    while(tmp){
        memcpy(&sbuf, sdata, sizeof(sdata_t));
        sdata_key_forward(&sbuf, tmp->mlist->move);
        make_tree_lookup(&sbuf, tmp, tn, tbase);
        if(tmp->mlist->next){
            mvlist_t *expanded = mvlist_alloc();
            expanded->mlist = tmp->mlist->next;
            tmp->mlist->next = NULL;
            expanded->next = tmp->next;
            tmp->next = expanded;
        }
        tmp = tmp->next;
    }
    return sdata_mvlist_sort(list, sdata, disproof_number_comp);
}

/*
 * 有界探索の打ち切り判定。clock()を毎ノード読むと高くつくので、
 * ノード数を数えて一定間隔でだけ時計を見る。
 */
#define JSON_BOUNDED_CLOCK_INTERVAL 4096

static bool json_bounded_budget_exhausted(void)
{
    if(!st_json_bounded_active) return false;
    st_json_bounded_nodes++;
    if(st_json_bounded_node_budget &&
       st_json_bounded_nodes > st_json_bounded_node_budget)
        return true;
    if(st_json_bounded_deadline_set &&
       !(st_json_bounded_nodes % JSON_BOUNDED_CLOCK_INTERVAL) &&
       clock() > st_json_bounded_deadline)
        return true;
    return false;
}

static json_bounded_mate_status_t json_bounded_mate_or(
    const sdata_t *sdata, unsigned int remaining, tbase_t *tbase)
{
    if(g_suspend || g_stop_received) return JSON_BOUNDED_ABORTED;
    if(json_bounded_budget_exhausted()) return JSON_BOUNDED_ABORTED;
    if(remaining == 0) return JSON_BOUNDED_NO_MATE;

    mvlist_t *list = generate_check(sdata, tbase);
    mvlist_t *candidate = list;
    json_bounded_mate_status_t result = JSON_BOUNDED_NO_MATE;
    while(candidate){
        sdata_t child;
        memcpy(&child, sdata, sizeof(sdata_t));
        sdata_move_forward(&child, candidate->mlist->move);
        json_bounded_mate_status_t child_result =
            json_bounded_mate_and(&child, remaining - 1, tbase);
        if(child_result != JSON_BOUNDED_NO_MATE){
            result = child_result;
            break;
        }
        candidate = candidate->next;
    }
    if(list) mvlist_free(list);
    return result;
}

static json_bounded_mate_status_t json_bounded_mate_and(
    const sdata_t *sdata, unsigned int remaining, tbase_t *tbase)
{
    if(g_suspend || g_stop_received) return JSON_BOUNDED_ABORTED;
    if(json_bounded_budget_exhausted()) return JSON_BOUNDED_ABORTED;

    /*
     * generate_evasion が同一の無駄合系列を mlist->next にまとめる。
     * 固定手数比較ではその系列を個別の2手として数えず、代表だけを調べる。
     */
    mvlist_t *list = generate_evasion(sdata, tbase);
    if(!list) return JSON_BOUNDED_MATE;
    if(remaining == 0){
        mvlist_free(list);
        return JSON_BOUNDED_NO_MATE;
    }

    mvlist_t *evasion = list;
    json_bounded_mate_status_t result = JSON_BOUNDED_MATE;
    while(evasion){
        sdata_t child;
        memcpy(&child, sdata, sizeof(sdata_t));
        sdata_move_forward(&child, evasion->mlist->move);
        json_bounded_mate_status_t child_result =
            json_bounded_mate_or(&child, remaining - 1, tbase);
        if(child_result != JSON_BOUNDED_MATE){
            result = child_result;
            break;
        }
        evasion = evasion->next;
    }
    mvlist_free(list);
    return result;
}

/*
 * 有界不詰の証拠として、詰みを免れる受方の応手を1手だけ取り出す。
 * 固定深さ内で詰まないことを示す代表手であり、完全な逃れ手順ではない。
 * 一意性(他の応手でも逃れられるか)はここでは判定しない。
 */
static json_bounded_mate_status_t json_bounded_no_mate_witness(
    const sdata_t *sdata, unsigned int remaining, tbase_t *tbase,
    move_t *witness, bool *witness_found)
{
    *witness_found = false;
    if(g_suspend || g_stop_received) return JSON_BOUNDED_ABORTED;
    if(json_bounded_budget_exhausted()) return JSON_BOUNDED_ABORTED;

    mvlist_t *list = generate_evasion(sdata, tbase);
    if(!list) return JSON_BOUNDED_MATE;
    if(remaining == 0){
        mvlist_free(list);
        return JSON_BOUNDED_NO_MATE;
    }

    mvlist_t *evasion = list;
    json_bounded_mate_status_t result = JSON_BOUNDED_MATE;
    while(evasion){
        sdata_t child;
        memcpy(&child, sdata, sizeof(sdata_t));
        sdata_move_forward(&child, evasion->mlist->move);
        json_bounded_mate_status_t child_result =
            json_bounded_mate_or(&child, remaining - 1, tbase);
        if(child_result != JSON_BOUNDED_MATE){
            result = child_result;
            if(child_result == JSON_BOUNDED_NO_MATE){
                *witness = evasion->mlist->move;
                *witness_found = true;
            }
            break;
        }
        evasion = evasion->next;
    }
    mvlist_free(list);
    return result;
}

static bool json_exact_mate_or(
    const sdata_t *sdata, unsigned int remaining, tbase_t *tbase,
    bool *aborted)
{
    json_bounded_mate_status_t within =
        json_bounded_mate_or(sdata, remaining, tbase);
    if(within == JSON_BOUNDED_ABORTED){
        *aborted = true;
        return false;
    }
    if(within != JSON_BOUNDED_MATE) return false;
    if(!remaining) return true;
    json_bounded_mate_status_t shorter =
        json_bounded_mate_or(sdata, remaining - 1, tbase);
    if(shorter == JSON_BOUNDED_ABORTED){
        *aborted = true;
        return false;
    }
    return shorter != JSON_BOUNDED_MATE;
}

static bool json_exact_mate_and(
    const sdata_t *sdata, unsigned int remaining, tbase_t *tbase,
    bool *aborted)
{
    json_bounded_mate_status_t within =
        json_bounded_mate_and(sdata, remaining, tbase);
    if(within == JSON_BOUNDED_ABORTED){
        *aborted = true;
        return false;
    }
    if(within != JSON_BOUNDED_MATE) return false;
    if(!remaining) return true;
    json_bounded_mate_status_t shorter =
        json_bounded_mate_and(sdata, remaining - 1, tbase);
    if(shorter == JSON_BOUNDED_ABORTED){
        *aborted = true;
        return false;
    }
    return shorter != JSON_BOUNDED_MATE;
}

static void json_emit_move(FILE *stream, move_t move, bool *first_move)
{
    char move_string[16];
    move_to_sfen(move_string, move);
    if(!*first_move) fprintf(stream, ",");
    fprintf(stream, "\"%s\"", move_string);
    *first_move = false;
}

static bool json_copy_file(FILE *destination, FILE *source)
{
    char buffer[1024];
    size_t count;
    rewind(source);
    while((count = fread(buffer, 1, sizeof(buffer), source)) > 0){
        if(fwrite(buffer, 1, count, destination) != count) return false;
    }
    return !ferror(source);
}

static char* json_read_stream(FILE *stream)
{
    if(fflush(stream) != 0 || fseek(stream, 0, SEEK_END) != 0) return NULL;
    long length = ftell(stream);
    if(length < 0 || fseek(stream, 0, SEEK_SET) != 0) return NULL;
    char *text = malloc((size_t)length + 1);
    if(!text) return NULL;
    size_t read_length = fread(text, 1, (size_t)length, stream);
    if(read_length != (size_t)length && ferror(stream)){
        free(text);
        return NULL;
    }
    text[read_length] = '\0';
    return text;
}

static bool json_position_seen(
    const sdata_t *sdata, const json_position_key_t *positions,
    unsigned int count)
{
    for(unsigned int i=0; i<count; i++){
        if(positions[i].zkey == S_ZKEY(sdata) &&
           positions[i].turn == S_TURN(sdata) &&
           !memcmp(&positions[i].sente_hand, &S_SMKEY(sdata),
                   sizeof(mkey_t)) &&
           !memcmp(&positions[i].gote_hand, &S_GMKEY(sdata),
                   sizeof(mkey_t)))
            return true;
    }
    return false;
}

/*
 * 固定済みの代案王手を指した直後(AND節点)から、不詰の反証木を一本たどる。
 * AND節点では dn==0 の受方逃れを、OR節点では dn==0 の代表王手を選ぶ。
 */
static json_no_mate_line_result_t json_emit_no_mate_line(
    FILE *stream, const sdata_t *sdata, tbase_t *tbase)
{
    json_no_mate_line_result_t result = {
        false, "disproof_tree_incomplete"
    };
    json_position_key_t positions[JSON_NO_MATE_LINE_MAX_PLIES + 1];
    unsigned int position_count = 0;
    unsigned int emitted = 0;
    bool defender_node = true;
    bool first_move = true;
    sdata_t current;
    memcpy(&current, sdata, sizeof(sdata_t));

    while(true){
        if(json_position_seen(&current, positions, position_count)){
            result.complete = true;
            result.terminal_reason = "repetition";
            return result;
        }
        positions[position_count++] = (json_position_key_t){
            S_ZKEY(&current), S_SMKEY(&current), S_GMKEY(&current),
            S_TURN(&current)
        };
        if(emitted >= JSON_NO_MATE_LINE_MAX_PLIES ||
           S_COUNT(&current) >= TSUME_MAX_DEPTH){
            result.terminal_reason = "maximum_depth_reached";
            return result;
        }
        if(g_suspend || g_stop_received){
            result.terminal_reason = "search_aborted";
            return result;
        }

        mvlist_t *list = defender_node
            ? json_prepare_and(&current, tbase)
            : json_prepare_or(&current, tbase);
        if(!list){
            if(!defender_node){
                result.complete = true;
                result.terminal_reason = "no_legal_checking_move";
            } else {
                result.terminal_reason = "unexpected_no_evasion";
            }
            return result;
        }

        mvlist_t *selected = list;
        while(selected && selected->tdata.dn != 0)
            selected = selected->next;
        if(!selected){
            mvlist_free(list);
            return result;
        }

        json_emit_move(stream, selected->mlist->move, &first_move);
        sdata_move_forward(&current, selected->mlist->move);
        emitted++;
        defender_node = !defender_node;
        mvlist_free(list);
    }
}

/*
 * 王手候補1手を、全王手を対象にした基準で解析する。
 * 未解決ならDFPNで追加探索し、それでも決まらなければ有界不詰を試す。
 * 不詰が付いた候補には代表反証線を添える。
 * 限定打の比較(alternative_details)には手を入れない。
 */
static void json_analyze_general_alternative(
    const sdata_t *sdata, mvlist_t *candidate, tdata_t threshold,
    tbase_t *tbase, json_general_alternative_t *out)
{
    out->move = candidate->mlist->move;
    out->result = candidate->tdata;
    out->horizon_out = false;
    out->refutation_line = NULL;
    out->refutation_complete = false;
    out->refutation_reason = NULL;

    sdata_t child;
    memcpy(&child, sdata, sizeof(sdata_t));
    sdata_move_forward(&child, out->move);
    tbase_t *candidate_tbase = tbase;

    if(out->result.pn != 0 && out->result.dn != 0 &&
       st_json_exclusivity_tbase){
        initialize_tbase(st_json_exclusivity_tbase);
        mvlist_t probe = *candidate;
        probe.next = NULL;
        probe.tdata = (tdata_t){1, 1, 0};
        probe.hinc = 0;
        probe.inc = 0;
        probe.nouse = 0;
        probe.nouse2 = 0;
        bns_and_isolated(
            &child, &threshold, &probe, st_json_exclusivity_tbase);
        out->result = probe.tdata;
        candidate_tbase = st_json_exclusivity_tbase;
        if(st_json_bounded_plies > 1 &&
           out->result.pn != 0 && out->result.dn != 0){
            initialize_tbase(st_json_exclusivity_tbase);
            move_t witness_move;
            bool witness_found = false;
            if(st_json_bounded_seconds > 0.0 &&
               !st_json_bounded_deadline_set){
                st_json_bounded_deadline = clock() + (clock_t)
                    (st_json_bounded_seconds * CLOCKS_PER_SEC);
                st_json_bounded_deadline_set = true;
            }
            st_json_bounded_nodes = 0;
            st_json_bounded_active = true;
            json_bounded_mate_status_t bounded_result =
                json_bounded_no_mate_witness(
                    &child, st_json_bounded_plies - 1,
                    st_json_exclusivity_tbase,
                    &witness_move, &witness_found);
            st_json_bounded_active = false;
            if(bounded_result == JSON_BOUNDED_NO_MATE){
                out->horizon_out = true;
                if(witness_found){
                    char witness_string[16];
                    move_to_sfen(witness_string, witness_move);
                    char *line = malloc(strlen(witness_string)+3);
                    if(line){
                        sprintf(line, "\"%s\"", witness_string);
                        out->refutation_line = line;
                        out->refutation_reason =
                            "bounded_no_mate_witness";
                    }
                }
            }
        }
    }

    if(out->result.dn == 0){
        FILE *refutation_stream = tmpfile();
        if(refutation_stream){
            json_no_mate_line_result_t refutation =
                json_emit_no_mate_line(
                    refutation_stream, &child, candidate_tbase);
            out->refutation_line = json_read_stream(refutation_stream);
            out->refutation_complete = refutation.complete;
            out->refutation_reason = refutation.terminal_reason;
            if(!out->refutation_line){
                out->refutation_complete = false;
                out->refutation_reason = "output_unavailable";
            }
            fclose(refutation_stream);
        } else {
            out->refutation_reason = "output_unavailable";
        }
    }
    return;
}

static mvlist_t* json_select_move(mvlist_t *list, move_t move)
{
    mvlist_t *selected = list;
    while(selected){
        if(selected->mlist->move.prev_pos == move.prev_pos &&
           selected->mlist->move.new_pos == move.new_pos) return selected;
        selected = selected->next;
    }
    return NULL;
}

/*
 * ある王手可能局面(OR節点)で、実際に選択された着手(selected_move)以外の
 * 合法な王手候補を全列挙し、それぞれが詰みに至るかどうかを判定する。
 *
 * 判定方針(証明木の不在と不詰を混同しない):
 *  - 局面表に既に pn==0(証明済み詰み)の候補は、そのまま「詰みと証明済み」
 *    として扱う。
 *  - pn!=0 かつ dn!=0(未解決)の候補のみ、既存の bns_and を用いて追加探索し、
 *    証明/反証を試みる。深さ上限は選択済み手順の詰み手数
 *    (list->tdata.sh、既存の json_optimize_or と同様の考え方)に
 *    JSON_EXCLUSIVITY_DEPTH_MARGIN を加えた値とする。TSUME_MAX_DEPTH
 *    まで無制限に深追いすると、選択されなかった候補の「不詰の証明」に
 *    要する計算量が非現実的に大きくなるため(証明対象と異なり反証は
 *    全分岐の消尽を要する)、意図的に有限の深さで打ち切る。
 *  - 追加探索後も pn!=0 かつ dn!=0 のままであれば「未解決」として扱い、
 *    不詰とはみなさない。
 *  - dn==0(反証済み不詰)の候補は「不詰と証明済み」として扱う。
 * 選択済みの手順・選択ロジック自体には一切変更を加えない(探索対象は
 * 選択されなかった候補のみ)。
 */
static json_exclusivity_entry_t* json_analyze_or_exclusivity(
    const sdata_t *sdata, tbase_t *tbase, mvlist_t *list,
    move_t selected_move, unsigned int ply)
{
    json_exclusivity_entry_t *entry = malloc(sizeof(json_exclusivity_entry_t));
    if(!entry) return NULL;
    entry->ply = ply;
    entry->move = selected_move;
    entry->status = JSON_EXCLUSIVITY_EXCLUSIVE;
    entry->same_piece_drop_scope = (
        MV_DROP(selected_move) &&
        (MV_HAND(selected_move) == KY ||
         MV_HAND(selected_move) == KA ||
         MV_HAND(selected_move) == HI)
    );
    entry->alternative_count = 0;
    entry->selected_mate_moves = list->tdata.sh;
    entry->comparison_search_plies = 0;
    entry->has_unresolved_alternatives = false;
    entry->proven_mating_count = 0;
    entry->general_count = 0;
    memset(entry->general_alternatives, 0,
           sizeof(entry->general_alternatives));
    for(unsigned int i=0; i<JSON_EXCLUSIVITY_MAX_ALT; i++){
        entry->alternative_bounded_out[i] = false;
        entry->alternative_horizon_out[i] = false;
        entry->alternative_refutation_lines[i] = NULL;
        entry->alternative_refutation_complete[i] = false;
        entry->alternative_refutation_reasons[i] = NULL;
    }
    for(unsigned int i=0; i<JSON_EXCLUSIVITY_MAX_ALT; i++)
        entry->proven_mating_lines[i] = NULL;
    entry->next = NULL;

    sdata_t selected_child;
    memcpy(&selected_child, sdata, sizeof(sdata_t));
    sdata_move_forward(&selected_child, selected_move);
    FILE *selected_stream = tmpfile();
    if(selected_stream){
        bool first_selected_move = true;
        json_line_result_t selected_result = json_emit_line_and(
            selected_stream, &selected_child, tbase,
            &first_selected_move, true, false);
        if(selected_result.complete && selected_result.mate)
            entry->selected_mate_moves =
                selected_result.terminal_ply - ply + 1;
        fclose(selected_stream);
    }

    bool same_piece_drop_scope = entry->same_piece_drop_scope;
    bool any_unresolved = false;
    bool selected_bound_found = !same_piece_drop_scope;
    unsigned int selected_bound_remaining =
        entry->selected_mate_moves > 0
            ? entry->selected_mate_moves - 1
            : 0;
    if(same_piece_drop_scope && st_json_exclusivity_tbase){
        unsigned int max_remaining = selected_bound_remaining +
            JSON_EXCLUSIVITY_DEPTH_MARGIN;
        if(max_remaining > TSUME_MAX_DEPTH ||
           max_remaining < selected_bound_remaining)
            max_remaining = TSUME_MAX_DEPTH;
        for(unsigned int remaining = selected_bound_remaining;
            remaining <= max_remaining;
            remaining++){
            initialize_tbase(st_json_exclusivity_tbase);
            json_bounded_mate_status_t selected_result =
                json_bounded_mate_and(
                    &selected_child, remaining,
                    st_json_exclusivity_tbase);
            if(selected_result == JSON_BOUNDED_MATE){
                selected_bound_remaining = remaining;
                selected_bound_found = true;
                entry->comparison_search_plies = remaining + 1;
                break;
            }
            if(selected_result == JSON_BOUNDED_ABORTED) break;
        }
    }
    unsigned int depth_limit = same_piece_drop_scope
        ? entry->selected_mate_moves
        : list->tdata.sh + JSON_EXCLUSIVITY_DEPTH_MARGIN;
    if(depth_limit > TSUME_MAX_DEPTH || depth_limit < list->tdata.sh)
        depth_limit = TSUME_MAX_DEPTH;
    tdata_t threshold = {INFINATE-1, INFINATE-1, depth_limit};
    mvlist_t *candidate = list;
    while(candidate){
        if(same_piece_drop_scope &&
           candidate->mlist->move.prev_pos != selected_move.prev_pos){
            candidate = candidate->next;
            continue;
        }
        if(candidate->mlist->move.prev_pos != selected_move.prev_pos ||
           candidate->mlist->move.new_pos  != selected_move.new_pos){
            unsigned int alternative_index = entry->alternative_count++;
            tdata_t candidate_result = candidate->tdata;
            tbase_t *candidate_tbase = tbase;
            sdata_t child;
            memcpy(&child, sdata, sizeof(sdata_t));
            sdata_move_forward(&child, candidate->mlist->move);
            bool bounded_out = false;
            bool bounded_mate = false;
            bool bounded_no_mate = false;
            if(same_piece_drop_scope && st_json_exclusivity_tbase &&
               selected_bound_found){
                initialize_tbase(st_json_exclusivity_tbase);
                json_bounded_mate_status_t bounded_result =
                    json_bounded_mate_and(
                        &child, selected_bound_remaining,
                        st_json_exclusivity_tbase);
                if(bounded_result == JSON_BOUNDED_MATE){
                    candidate_result =
                        (tdata_t){0, INFINATE-1,
                                  entry->selected_mate_moves};
                    bounded_mate = true;
                } else if(bounded_result == JSON_BOUNDED_NO_MATE){
                    initialize_tbase(st_json_exclusivity_tbase);
                    mvlist_t probe = *candidate;
                    tdata_t proof_threshold = {
                        JSON_EXCLUSIVITY_PROOF_PN_LIMIT,
                        INFINATE-1,
                        TSUME_MAX_DEPTH
                    };
                    probe.next = NULL;
                    probe.tdata = (tdata_t){1, 1, 0};
                    probe.hinc = 0;
                    probe.inc = 0;
                    probe.nouse = 0;
                    probe.nouse2 = 0;
                    bns_and_isolated(
                        &child, &proof_threshold, &probe,
                        st_json_exclusivity_tbase);
                    candidate_tbase = st_json_exclusivity_tbase;
                    if(probe.tdata.pn == 0){
                        candidate_result = probe.tdata;
                    } else {
                        candidate_result =
                            (tdata_t){probe.tdata.pn,
                                      probe.tdata.dn,
                                      entry->selected_mate_moves};
                        bounded_out = true;
                    }
                }
            } else if(same_piece_drop_scope){
                candidate_result = (tdata_t){1, 1, 0};
            } else if(
                candidate_result.pn != 0 &&
                candidate_result.dn != 0 &&
                st_json_exclusivity_tbase){
                initialize_tbase(st_json_exclusivity_tbase);
                mvlist_t probe = *candidate;
                probe.next = NULL;
                probe.tdata = (tdata_t){1, 1, 0};
                probe.hinc = 0;
                probe.inc = 0;
                probe.nouse = 0;
                probe.nouse2 = 0;
                bns_and_isolated(
                    &child, &threshold, &probe,
                    st_json_exclusivity_tbase);
                candidate_result = probe.tdata;
                candidate_tbase = st_json_exclusivity_tbase;
                /*
                 * DFPNでも未解決のまま残った代案は、完全な不詰証明の
                 * 代わりに「初手から数えて指定手数以内には詰まない」ことを
                 * 固定深さの全探索で確かめる。全分岐の消尽を要する完全
                 * 反証と違い、深さが有限なので必ず停止する。
                 * 手数は分岐からの相対で数える。選択手順の詰み手数と揃える
                 * 意味は無く、紛れは「その手を指してから読者が読む範囲で
                 * 詰まないか」が問題であるため。
                 */
                if(st_json_bounded_plies > 1 &&
                   candidate_result.pn != 0 &&
                   candidate_result.dn != 0){
                    initialize_tbase(st_json_exclusivity_tbase);
                    move_t witness_move;
                    bool witness_found = false;
                    if(st_json_bounded_seconds > 0.0 &&
                       !st_json_bounded_deadline_set){
                        st_json_bounded_deadline = clock() + (clock_t)
                            (st_json_bounded_seconds * CLOCKS_PER_SEC);
                        st_json_bounded_deadline_set = true;
                    }
                    st_json_bounded_nodes = 0;
                    st_json_bounded_active = true;
                    json_bounded_mate_status_t bounded_result =
                        json_bounded_no_mate_witness(
                            &child, st_json_bounded_plies - 1,
                            st_json_exclusivity_tbase,
                            &witness_move, &witness_found);
                    st_json_bounded_active = false;
                    if(bounded_result == JSON_BOUNDED_NO_MATE){
                        bounded_no_mate = true;
                        if(witness_found &&
                           alternative_index < JSON_EXCLUSIVITY_MAX_ALT){
                            char witness_string[16];
                            move_to_sfen(witness_string, witness_move);
                            char *line = malloc(strlen(witness_string)+3);
                            if(line){
                                sprintf(line, "\"%s\"", witness_string);
                                entry->alternative_refutation_lines[
                                    alternative_index] = line;
                                entry->alternative_refutation_complete[
                                    alternative_index] = false;
                                entry->alternative_refutation_reasons[
                                    alternative_index] =
                                    "bounded_no_mate_witness";
                            }
                        }
                    }
                }
            }
            if(alternative_index < JSON_EXCLUSIVITY_MAX_ALT){
                entry->alternatives[alternative_index] =
                    candidate->mlist->move;
                entry->alternative_results[alternative_index] =
                    candidate_result;
                entry->alternative_bounded_out[alternative_index] =
                    bounded_out;
                entry->alternative_horizon_out[alternative_index] =
                    bounded_no_mate;
            }
            if(candidate_result.pn == 0){
                bool mating_line_complete = bounded_mate;
                if(entry->proven_mating_count < JSON_EXCLUSIVITY_MAX_ALT){
                    unsigned int mating_index =
                        entry->proven_mating_count;
                    entry->proven_mating[entry->proven_mating_count] =
                        candidate->mlist->move;
                    entry->proven_mating_moves[mating_index] =
                        candidate_result.sh;
                    memcpy(&child, sdata, sizeof(sdata_t));
                    sdata_move_forward(
                        &child, candidate->mlist->move);
                    FILE *line_stream = bounded_mate ? NULL : tmpfile();
                    if(line_stream){
                        bool first_line_move = true;
                        json_line_result_t line_result =
                            json_emit_line_and(
                                line_stream, &child,
                                candidate_tbase, &first_line_move,
                                true, false);
                        if(line_result.complete && line_result.mate){
                            mating_line_complete = true;
                            entry->proven_mating_moves[mating_index] =
                                line_result.terminal_ply - ply + 1;
                        }
                        fflush(line_stream);
                        if(fseek(line_stream, 0, SEEK_END) == 0){
                            long line_length = ftell(line_stream);
                            if(line_length >= 0 &&
                               fseek(line_stream, 0, SEEK_SET) == 0){
                                char *line = malloc(
                                    (size_t)line_length + 1);
                                if(line){
                                    size_t read_length = fread(
                                        line, 1, (size_t)line_length,
                                        line_stream);
                                    line[read_length] = '\0';
                                    entry->proven_mating_lines[
                                        mating_index
                                    ] = line;
                                }
                            }
                        }
                        fclose(line_stream);
                    }
                }
                if(mating_line_complete){
                    entry->proven_mating_count++;
                } else {
                    if(entry->proven_mating_count <
                       JSON_EXCLUSIVITY_MAX_ALT){
                        unsigned int mating_index =
                            entry->proven_mating_count;
                        free(entry->proven_mating_lines[mating_index]);
                        entry->proven_mating_lines[mating_index] = NULL;
                    }
                    candidate_result =
                        (tdata_t){1, 1, entry->selected_mate_moves};
                    bounded_out = true;
                    if(alternative_index < JSON_EXCLUSIVITY_MAX_ALT){
                        entry->alternative_results[alternative_index] =
                            candidate_result;
                        entry->alternative_bounded_out[alternative_index] =
                            true;
                    }
                }
            } else if(
                candidate_result.dn != 0 &&
                !bounded_out){
                /* pn!=0 かつ dn!=0: 追加探索でも未解決のまま */
                any_unresolved = true;
            }
            if(candidate_result.dn == 0 &&
               alternative_index < JSON_EXCLUSIVITY_MAX_ALT){
                FILE *refutation_stream = tmpfile();
                if(refutation_stream){
                    json_no_mate_line_result_t refutation =
                        json_emit_no_mate_line(
                            refutation_stream, &child, candidate_tbase);
                    entry->alternative_refutation_lines[alternative_index] =
                        json_read_stream(refutation_stream);
                    entry->alternative_refutation_complete[alternative_index] =
                        refutation.complete;
                    entry->alternative_refutation_reasons[alternative_index] =
                        refutation.terminal_reason;
                    if(!entry->alternative_refutation_lines[
                           alternative_index]){
                        entry->alternative_refutation_complete[
                            alternative_index] = false;
                        entry->alternative_refutation_reasons[
                            alternative_index] = "output_unavailable";
                    }
                    fclose(refutation_stream);
                } else {
                    entry->alternative_refutation_reasons[
                        alternative_index] = "output_unavailable";
                }
            }
        }
        candidate = candidate->next;
    }
    /*
     * 限定打の比較に切り替わった手数では、上のループが同駒種の打しか見ていない。
     * 紛れの候補には残りの王手も要るので、全王手を対象にした解析を別に行う。
     * status と alternative_details は限定打の判定のまま触らない。
     */
    if(same_piece_drop_scope){
        unsigned int general_depth =
            list->tdata.sh + JSON_EXCLUSIVITY_DEPTH_MARGIN;
        if(general_depth > TSUME_MAX_DEPTH ||
           general_depth < list->tdata.sh)
            general_depth = TSUME_MAX_DEPTH;
        tdata_t general_threshold = {
            INFINATE-1, INFINATE-1, general_depth
        };
        for(mvlist_t *node = list; node; node = node->next){
            if(node->mlist->move.prev_pos == selected_move.prev_pos &&
               node->mlist->move.new_pos  == selected_move.new_pos)
                continue;
            if(entry->general_count >= JSON_EXCLUSIVITY_MAX_ALT) break;
            json_analyze_general_alternative(
                sdata, node, general_threshold, tbase,
                &entry->general_alternatives[entry->general_count++]);
        }
    }
    if(entry->proven_mating_count > 0)
        entry->status = JSON_EXCLUSIVITY_NONEXCLUSIVE;
    else if(any_unresolved)
        entry->status = JSON_EXCLUSIVITY_UNVERIFIED;
    else
        entry->status = JSON_EXCLUSIVITY_EXCLUSIVE;
    entry->has_unresolved_alternatives = any_unresolved;
    return entry;
}

static void json_free_exclusivity(json_exclusivity_entry_t *list)
{
    while(list){
        json_exclusivity_entry_t *next = list->next;
        unsigned int shown = MIN(
            list->proven_mating_count,
            JSON_EXCLUSIVITY_MAX_ALT);
        for(unsigned int i=0; i<shown; i++)
            free(list->proven_mating_lines[i]);
        for(unsigned int i=0; i<JSON_EXCLUSIVITY_MAX_ALT; i++)
            free(list->alternative_refutation_lines[i]);
        for(unsigned int i=0; i<JSON_EXCLUSIVITY_MAX_ALT; i++)
            free(list->general_alternatives[i].refutation_line);
        free(list);
        list = next;
    }
}

static const char* json_exclusivity_status_string(
    json_exclusivity_status_t status)
{
    switch(status){
        case JSON_EXCLUSIVITY_NONEXCLUSIVE: return "nonexclusive";
        case JSON_EXCLUSIVITY_UNVERIFIED:   return "unverified";
        case JSON_EXCLUSIVITY_EXCLUSIVE:
        default:                            return "exclusive";
    }
}

static void json_print_exclusivity(
    FILE *stream, json_exclusivity_entry_t *list)
{
    fprintf(stream, "[");
    bool first_entry = true;
    while(list){
        char move_string[16];
        move_to_sfen(move_string, list->move);
        if(!first_entry) fprintf(stream, ",");
        fprintf(stream,
                "{\"ply\":%u,\"move\":\"%s\",\"status\":\"%s\""
                ",\"comparison_scope\":\"%s\""
                ",\"alternative_count\":%u"
                ",\"selected_mate_moves\":%u"
                ",\"comparison_search_plies\":%u"
                ",\"has_unresolved_alternatives\":%s"
                ",\"proven_mating_alternatives\":[",
                list->ply, move_string,
                json_exclusivity_status_string(list->status),
                list->same_piece_drop_scope
                    ? "same_piece_drop_within_selected_length"
                    : "all_checking_moves",
                list->alternative_count,
                list->selected_mate_moves,
                list->comparison_search_plies,
                list->has_unresolved_alternatives ? "true" : "false");
        bool first_alt = true;
        unsigned int shown = MIN(list->proven_mating_count,
                                  JSON_EXCLUSIVITY_MAX_ALT);
        for(unsigned int i=0; i<shown; i++){
            json_emit_move(stream, list->proven_mating[i], &first_alt);
        }
        fprintf(stream, "],\"proven_mating_alternative_details\":[");
        first_alt = true;
        for(unsigned int i=0; i<shown; i++){
            char alternative_string[16];
            move_to_sfen(alternative_string, list->proven_mating[i]);
            if(!first_alt) fprintf(stream, ",");
            fprintf(
                stream,
                "{\"move\":\"%s\",\"mate_moves\":%u,\"line\":[\"%s\"",
                alternative_string,
                list->proven_mating_moves[i],
                alternative_string);
            if(list->proven_mating_lines[i] &&
               list->proven_mating_lines[i][0] != '\0')
                fprintf(
                    stream, ",%s",
                    list->proven_mating_lines[i]);
            fprintf(stream, "]}");
            first_alt = false;
        }
        fprintf(stream, "],\"alternative_details\":[");
        first_alt = true;
        unsigned int alternative_shown = MIN(
            list->alternative_count, JSON_EXCLUSIVITY_MAX_ALT);
        for(unsigned int i=0; i<alternative_shown; i++){
            char alternative_string[16];
            move_to_sfen(alternative_string, list->alternatives[i]);
            if(!first_alt) fprintf(stream, ",");
            fprintf(
                stream,
                "{\"move\":\"%s\",\"status\":\"%s\""
                ",\"proof_number\":%u,\"disproof_number\":%u"
                ",\"search_depth\":%u",
                alternative_string,
                list->alternative_results[i].pn == 0
                    ? "mate"
                    : list->alternative_results[i].dn == 0
                    ? "no_mate"
                    : list->alternative_bounded_out[i]
                    ? "no_mate_within_selected_length"
                    : list->alternative_horizon_out[i]
                    ? "no_mate_within_horizon"
                    : "unresolved",
                list->alternative_results[i].pn,
                list->alternative_results[i].dn,
                list->alternative_results[i].sh);
            if(list->alternative_results[i].dn == 0 ||
               list->alternative_refutation_lines[i]){
                fprintf(stream, ",\"refutation_line\":[");
                if(list->alternative_refutation_lines[i] &&
                   list->alternative_refutation_lines[i][0] != '\0')
                    fprintf(stream, "%s",
                            list->alternative_refutation_lines[i]);
                fprintf(
                    stream,
                    "],\"refutation_line_complete\":%s"
                    ",\"refutation_terminal_reason\":\"%s\"",
                    list->alternative_refutation_complete[i]
                        ? "true" : "false",
                    list->alternative_refutation_reasons[i]
                        ? list->alternative_refutation_reasons[i]
                        : "output_unavailable");
            }
            fprintf(stream, "}");
            first_alt = false;
        }
        fprintf(stream, "]");
        if(list->general_count){
            fprintf(stream, ",\"general_alternative_details\":[");
            first_alt = true;
            unsigned int general_shown = MIN(
                list->general_count, JSON_EXCLUSIVITY_MAX_ALT);
            for(unsigned int i=0; i<general_shown; i++){
                json_general_alternative_t *alternative =
                    &list->general_alternatives[i];
                char alternative_string[16];
                move_to_sfen(alternative_string, alternative->move);
                if(!first_alt) fprintf(stream, ",");
                fprintf(
                    stream,
                    "{\"move\":\"%s\",\"status\":\"%s\""
                    ",\"proof_number\":%u,\"disproof_number\":%u"
                    ",\"search_depth\":%u",
                    alternative_string,
                    alternative->result.pn == 0
                        ? "mate"
                        : alternative->result.dn == 0
                        ? "no_mate"
                        : alternative->horizon_out
                        ? "no_mate_within_horizon"
                        : "unresolved",
                    alternative->result.pn,
                    alternative->result.dn,
                    alternative->result.sh);
                if(alternative->result.dn == 0 ||
                   alternative->refutation_line){
                    fprintf(stream, ",\"refutation_line\":[");
                    if(alternative->refutation_line &&
                       alternative->refutation_line[0] != '\0')
                        fprintf(stream, "%s",
                                alternative->refutation_line);
                    fprintf(
                        stream,
                        "],\"refutation_line_complete\":%s"
                        ",\"refutation_terminal_reason\":\"%s\"",
                        alternative->refutation_complete
                            ? "true" : "false",
                        alternative->refutation_reason
                            ? alternative->refutation_reason
                            : "output_unavailable");
                }
                fprintf(stream, "}");
                first_alt = false;
            }
            fprintf(stream, "]");
        }
        fprintf(stream, "}");
        first_entry = false;
        list = list->next;
    }
    fprintf(stream, "]");
}

bool tsume_json_defender_line_fprint(
    FILE *stream, const sdata_t *sdata, tbase_t *tbase)
{
    bool first_move = true;
    json_line_result_t result = json_emit_line_and(
        stream, sdata, tbase, &first_move, false, false);
    return result.complete && result.mate;
}

void tsume_json_defender_variations_fprint(
    FILE *stream, const sdata_t *sdata, tbase_t *tbase)
{
    fprintf(stream, "[");
    mvlist_t *list = json_prepare_and(sdata, tbase);
    bool first_variation = true;
    mvlist_t *candidate = list;
    while(candidate){
        if(candidate->tdata.pn){
            sdata_t recovery;
            tdata_t threshold = {
                INFINATE-1, INFINATE-1, TSUME_MAX_DEPTH
            };
            memcpy(&recovery, sdata, sizeof(sdata_t));
            sdata_move_forward(
                &recovery, candidate->mlist->move);
            bns_or(&recovery, &threshold, candidate, tbase);
        }
        if(!candidate->tdata.pn){
            char defender_move[16];
            move_to_sfen(
                defender_move, candidate->mlist->move);
            if(!first_variation) fprintf(stream, ",");
            fprintf(
                stream,
                "{\"branch_ply\":%u,\"defender_move\":\"%s\""
                ",\"line\":[\"%s\"",
                S_COUNT(sdata)+1, defender_move, defender_move);
            sdata_t child;
            memcpy(&child, sdata, sizeof(sdata_t));
            sdata_move_forward(
                &child, candidate->mlist->move);
            bool first_move = false;
            json_line_result_t result = json_emit_line_or(
                stream, &child, tbase, &first_move, false, false);
            fprintf(
                stream,
                "],\"terminal_ply\":%u,\"complete\":%s}",
                result.terminal_ply,
                result.complete && result.mate ? "true" : "false");
            first_variation = false;
        }
        candidate = candidate->next;
    }
    if(list) mvlist_free(list);
    fprintf(stream, "]");
}

static json_line_result_t json_emit_line_or(
    FILE *stream, const sdata_t *sdata, tbase_t *tbase, bool *first_move,
    bool research_lines, bool analyze_exclusivity)
{
    json_line_result_t result = {
        true, false, S_COUNT(sdata), 0, NULL, true
    };
    if(S_COUNT(sdata) >= TSUME_MAX_DEPTH){
        result.complete = false;
        result.exclusivity_complete = false;
        return result;
    }

    mvlist_t *list = json_prepare_or(sdata, tbase);
    if(research_lines && list && !list->tdata.pn)
        list = json_optimize_or(list, sdata, tbase);
    if(!list || list->tdata.pn){
        result.complete = false;
        result.exclusivity_complete = false;
        if(list) mvlist_free(list);
        return result;
    }

    move_t move = list->mlist->move;
    json_emit_move(stream, move, first_move);
    sdata_t sbuf;
    memcpy(&sbuf, sdata, sizeof(sdata_t));
    sdata_move_forward(&sbuf, move);
    result = json_emit_line_and(
        stream, &sbuf, tbase, first_move, research_lines,
        analyze_exclusivity);
    /* 手順を最後まで確定してから補助探索する。先に局面表を更新すると、
     * 再帰先の代表手順選択へ影響してしまう。 */
    json_exclusivity_entry_t *entry = analyze_exclusivity
        ? json_analyze_or_exclusivity(sdata, tbase, list, move,
                                       S_COUNT(sdata)+1)
        : NULL;
    if(entry){
        entry->next = result.exclusivity;
        result.exclusivity = entry;
    }
    if(analyze_exclusivity && !entry)
        result.exclusivity_complete = false;
    mvlist_free(list);
    return result;
}

static json_line_result_t json_emit_line_and(
    FILE *stream, const sdata_t *sdata, tbase_t *tbase, bool *first_move,
    bool research_lines, bool analyze_exclusivity)
{
    json_line_result_t result = {
        true, false, S_COUNT(sdata), 0, NULL, true
    };
    if(S_COUNT(sdata) >= TSUME_MAX_DEPTH){
        result.complete = false;
        result.exclusivity_complete = false;
        return result;
    }

    mvlist_t *list = json_prepare_and(sdata, tbase);
    if(!list){
        result.mate = true;
        result.attacker_hand_count = TOTAL_MKEY(ENEMY_MKEY(sdata));
        return result;
    }
    if(list->tdata.pn){
        result.complete = false;
        result.exclusivity_complete = false;
        mvlist_free(list);
        return result;
    }

    move_t move = list->mlist->move;
    json_emit_move(stream, move, first_move);
    sdata_t sbuf;
    memcpy(&sbuf, sdata, sizeof(sdata_t));
    sdata_move_forward(&sbuf, move);
    result = json_emit_line_or(
        stream, &sbuf, tbase, first_move, research_lines,
        analyze_exclusivity);
    mvlist_free(list);
    return result;
}

static json_line_result_t json_emit_optimal_or(
        FILE *stream, const sdata_t *sdata, tbase_t *tbase, bool *first_move,
        unsigned int remaining, bool analyze_exclusivity)
    {
        json_line_result_t failure = {
            false, false, S_COUNT(sdata), 0, NULL, false
        };
        if(!remaining || S_COUNT(sdata) >= TSUME_MAX_DEPTH) return failure;

        mvlist_t *list = json_prepare_or(sdata, tbase);
        FILE *best_stream = NULL;
        bool best_first = *first_move;
        move_t best_move = {0};
        json_line_result_t best = failure;
        bool aborted = false;
        mvlist_t *candidate = list;
        while(candidate){
            sdata_t child;
            memcpy(&child, sdata, sizeof(sdata_t));
            sdata_move_forward(&child, candidate->mlist->move);
            if(json_exact_mate_and(&child, remaining - 1, tbase, &aborted)){
                FILE *candidate_stream = tmpfile();
                if(!candidate_stream){
                    aborted = true;
                    break;
                }
                bool candidate_first = *first_move;
                json_emit_move(
                    candidate_stream, candidate->mlist->move, &candidate_first);
                json_line_result_t candidate_result = json_emit_optimal_and(
                    candidate_stream, &child, tbase, &candidate_first,
                    remaining - 1, false);
                if(!candidate_result.complete || !candidate_result.mate){
                    json_free_exclusivity(candidate_result.exclusivity);
                    fclose(candidate_stream);
                    aborted = true;
                    break;
                }
                if(!best_stream ||
                   candidate_result.attacker_hand_count >
                       best.attacker_hand_count){
                    if(best_stream) fclose(best_stream);
                    json_free_exclusivity(best.exclusivity);
                    best_stream = candidate_stream;
                    best_first = candidate_first;
                    best_move = candidate->mlist->move;
                    best = candidate_result;
                } else {
                    json_free_exclusivity(candidate_result.exclusivity);
                    fclose(candidate_stream);
                }
            }
            if(aborted) break;
            candidate = candidate->next;
        }

        if(aborted || !best_stream){
            if(best_stream) fclose(best_stream);
            json_free_exclusivity(best.exclusivity);
            if(list) mvlist_free(list);
            return failure;
        }
        if(analyze_exclusivity){
            fclose(best_stream);
            best_stream = tmpfile();
            if(!best_stream){
                json_free_exclusivity(best.exclusivity);
                mvlist_free(list);
                return failure;
            }
            best_first = *first_move;
            json_emit_move(best_stream, best_move, &best_first);
            sdata_t child;
            memcpy(&child, sdata, sizeof(sdata_t));
            sdata_move_forward(&child, best_move);
            best = json_emit_optimal_and(
                best_stream, &child, tbase, &best_first, remaining - 1, true);
            if(!best.complete || !best.mate){
                fclose(best_stream);
                json_free_exclusivity(best.exclusivity);
                mvlist_free(list);
                return failure;
            }
        }
        if(!json_copy_file(stream, best_stream)) best.complete = false;
        fclose(best_stream);
        *first_move = best_first;
        if(analyze_exclusivity){
            json_exclusivity_entry_t *entry = json_analyze_or_exclusivity(
                sdata, tbase, list, best_move, S_COUNT(sdata)+1);
            if(entry){
                entry->next = best.exclusivity;
                best.exclusivity = entry;
            } else {
                best.exclusivity_complete = false;
            }
        }
        mvlist_free(list);
        return best;
    }

    static json_line_result_t json_emit_optimal_and(
        FILE *stream, const sdata_t *sdata, tbase_t *tbase, bool *first_move,
        unsigned int remaining, bool analyze_exclusivity)
    {
        json_line_result_t failure = {
            false, false, S_COUNT(sdata), 0, NULL, false
        };
        mvlist_t *list = json_prepare_and(sdata, tbase);
        if(!list){
            json_line_result_t mate = {
                true, true, S_COUNT(sdata),
                TOTAL_MKEY(ENEMY_MKEY(sdata)), NULL, true
            };
            return remaining == 0 ? mate : failure;
        }
        if(!remaining){
            mvlist_free(list);
            return failure;
        }

        FILE *best_stream = NULL;
        bool best_first = *first_move;
        move_t best_move = {0};
        json_line_result_t best = failure;
        bool aborted = false;
        mvlist_t *candidate = list;
        while(candidate){
            sdata_t child;
            memcpy(&child, sdata, sizeof(sdata_t));
            sdata_move_forward(&child, candidate->mlist->move);
            if(json_exact_mate_or(&child, remaining - 1, tbase, &aborted)){
                FILE *candidate_stream = tmpfile();
                if(!candidate_stream){
                    aborted = true;
                    break;
                }
                bool candidate_first = *first_move;
                json_emit_move(
                    candidate_stream, candidate->mlist->move, &candidate_first);
                json_line_result_t candidate_result = json_emit_optimal_or(
                    candidate_stream, &child, tbase, &candidate_first,
                    remaining - 1, false);
                if(!candidate_result.complete || !candidate_result.mate){
                    json_free_exclusivity(candidate_result.exclusivity);
                    fclose(candidate_stream);
                    aborted = true;
                    break;
                }
                if(!best_stream ||
                   candidate_result.attacker_hand_count <
                       best.attacker_hand_count){
                    if(best_stream) fclose(best_stream);
                    json_free_exclusivity(best.exclusivity);
                    best_stream = candidate_stream;
                    best_first = candidate_first;
                    best_move = candidate->mlist->move;
                    best = candidate_result;
                } else {
                    json_free_exclusivity(candidate_result.exclusivity);
                    fclose(candidate_stream);
                }
            }
            if(aborted) break;
            candidate = candidate->next;
        }

        mvlist_free(list);
        if(aborted || !best_stream){
            if(best_stream) fclose(best_stream);
            json_free_exclusivity(best.exclusivity);
            return failure;
        }
        if(analyze_exclusivity){
            fclose(best_stream);
            best_stream = tmpfile();
            if(!best_stream){
                json_free_exclusivity(best.exclusivity);
                return failure;
            }
            best_first = *first_move;
            json_emit_move(best_stream, best_move, &best_first);
            sdata_t child;
            memcpy(&child, sdata, sizeof(sdata_t));
            sdata_move_forward(&child, best_move);
            best = json_emit_optimal_or(
                best_stream, &child, tbase, &best_first, remaining - 1, true);
            if(!best.complete || !best.mate){
                fclose(best_stream);
                json_free_exclusivity(best.exclusivity);
                return failure;
            }
        }
        if(!json_copy_file(stream, best_stream)) best.complete = false;
        fclose(best_stream);
        *first_move = best_first;
        return best;
}

static json_baseline_line_t* json_capture_baseline(
    unsigned int branch_ply, move_t defender_move,
    const sdata_t *branch, tbase_t *tbase, bool analyze_exclusivity)
{
    json_baseline_line_t *baseline = malloc(sizeof(json_baseline_line_t));
    if(!baseline) return NULL;
    baseline->branch_ply = branch_ply;
    baseline->defender_move = defender_move;
    baseline->stream = tmpfile();
    baseline->result = (json_line_result_t){
        false, false, branch_ply, 0, NULL, false
    };
    baseline->first_move = false;
    baseline->next = st_json_baselines;
    if(baseline->stream){
        baseline->result = json_emit_line_or(
            baseline->stream, branch, tbase,
            &baseline->first_move, false, analyze_exclusivity);
        if(!analyze_exclusivity)
            baseline->result.exclusivity_complete = false;
    }
    st_json_baselines = baseline;
    return baseline;
}

static json_baseline_line_t* json_find_baseline(
    unsigned int branch_ply, move_t defender_move)
{
    json_baseline_line_t *list = st_json_baselines;
    while(list &&
          (list->branch_ply != branch_ply ||
           list->defender_move.prev_pos != defender_move.prev_pos ||
           list->defender_move.new_pos != defender_move.new_pos))
        list = list->next;
    return list;
}

static void json_free_baselines(json_baseline_line_t *list)
{
    while(list){
        json_baseline_line_t *next = list->next;
        if(list->stream) fclose(list->stream);
        /* baselineが所有するexclusivityリストはここで解放する。
         * 実出力側で採用された場合もポインタを共有しているだけなので
         * 実出力側では解放しない(json_collect_variations_andを参照)。 */
        json_free_exclusivity(list->result.exclusivity);
        free(list);
        list = next;
    }
}

static json_line_result_t json_emit_researched_line_or(
    FILE *stream, const sdata_t *sdata, tbase_t *tbase, bool *first_move,
    json_baseline_line_t *baseline, bool *used_research,
    bool analyze_exclusivity)
{
    *used_research = false;
    if(!baseline || !baseline->stream)
        return json_emit_line_or(
            stream, sdata, tbase, first_move, false, analyze_exclusivity);

    FILE *research_stream = tmpfile();
    if(!research_stream){
        json_line_result_t result = baseline->result;
        *first_move = baseline->first_move;
        if(!json_copy_file(stream, baseline->stream)) result.complete = false;
        return result;
    }

    bool research_first = *first_move;
    unsigned int upper = TSUME_MAX_DEPTH - S_COUNT(sdata);
    unsigned int shortest = 0;
    bool aborted = false;
    for(unsigned int depth = 1; depth <= upper; depth += 2){
        json_bounded_mate_status_t bounded =
            json_bounded_mate_or(sdata, depth, tbase);
        if(bounded == JSON_BOUNDED_ABORTED){
            aborted = true;
            break;
        }
        if(bounded == JSON_BOUNDED_MATE){
            shortest = depth;
            break;
        }
    }
    json_line_result_t researched = {
        false, false, S_COUNT(sdata), 0, NULL, false
    };
    if(!aborted && shortest){
        researched = json_emit_optimal_or(
            research_stream, sdata, tbase, &research_first, shortest,
            analyze_exclusivity);
    }

    bool choose_research =
        researched.complete && researched.mate;
    FILE *selected_stream =
        choose_research ? research_stream : baseline->stream;
    json_line_result_t result =
        choose_research ? researched : baseline->result;
    *first_move =
        choose_research ? research_first : baseline->first_move;
    *used_research = choose_research;
    if(!json_copy_file(stream, selected_stream)) result.complete = false;

    fclose(research_stream);
    /* 採用されなかった側のexclusivityリストは、ここで確実に解放する。
     * baseline側が不採用の場合はjson_free_baselinesで後ほど解放される。 */
    if(!choose_research) json_free_exclusivity(researched.exclusivity);
    return result;
}

static bool json_collect_variations_or(
    FILE *stream, const sdata_t *sdata, tbase_t *tbase, bool *first_variation,
    const move_t *principal, unsigned int principal_length,
    bool research_lines, bool analyze_exclusivity, bool *principal_valid)
{
    if(S_COUNT(sdata) >= TSUME_MAX_DEPTH){
        *principal_valid = false;
        return false;
    }
    mvlist_t *list = json_prepare_or(sdata, tbase);
    if(!list || list->tdata.pn){
        *principal_valid = false;
        if(list) mvlist_free(list);
        return false;
    }

    mvlist_t *selected = list;
    if(principal && S_COUNT(sdata) >= principal_length){
        *principal_valid = false;
        mvlist_free(list);
        return false;
    }
    if(principal && S_COUNT(sdata) < principal_length){
        selected = json_select_move(list, principal[S_COUNT(sdata)]);
    }
    if(selected && selected->tdata.pn){
        sdata_t recovery;
        tdata_t threshold = {INFINATE-1, INFINATE-1, TSUME_MAX_DEPTH};
        memcpy(&recovery, sdata, sizeof(sdata_t));
        sdata_move_forward(&recovery, selected->mlist->move);
        bns_and(&recovery, &threshold, selected, tbase);
    }
    if(!selected || selected->tdata.pn){
        *principal_valid = false;
        mvlist_free(list);
        return false;
    }

    sdata_t sbuf;
    memcpy(&sbuf, sdata, sizeof(sdata_t));
    sdata_move_forward(&sbuf, selected->mlist->move);
    bool complete = json_collect_variations_and(
        stream, &sbuf, tbase, first_variation, principal, principal_length,
        research_lines, analyze_exclusivity, principal_valid);
    if(analyze_exclusivity){
        json_exclusivity_entry_t *entry = json_analyze_or_exclusivity(
            sdata, tbase, list, selected->mlist->move, S_COUNT(sdata)+1);
        if(entry){
            entry->next = st_json_principal_exclusivity;
            st_json_principal_exclusivity = entry;
        }
    }
    mvlist_free(list);
    return complete;
}

static bool json_collect_variations_and(
    FILE *stream, const sdata_t *sdata, tbase_t *tbase, bool *first_variation,
    const move_t *principal, unsigned int principal_length,
    bool research_lines, bool analyze_exclusivity, bool *principal_valid)
{
    if(S_COUNT(sdata) >= TSUME_MAX_DEPTH){
        *principal_valid = false;
        return false;
    }
    mvlist_t *list = json_prepare_and(sdata, tbase);
    if(!list){
        if(principal && S_COUNT(sdata) != principal_length){
            *principal_valid = false;
            return false;
        }
        return true;
    }

    mvlist_t *selected = list;
    if(principal && S_COUNT(sdata) >= principal_length){
        *principal_valid = false;
        mvlist_free(list);
        return false;
    }
    if(principal && S_COUNT(sdata) < principal_length){
        selected = json_select_move(list, principal[S_COUNT(sdata)]);
    }
    if(selected && selected->tdata.pn){
        sdata_t recovery;
        tdata_t threshold = {INFINATE-1, INFINATE-1, TSUME_MAX_DEPTH};
        memcpy(&recovery, sdata, sizeof(sdata_t));
        sdata_move_forward(&recovery, selected->mlist->move);
        bns_or(&recovery, &threshold, selected, tbase);
    }
    if(!selected || selected->tdata.pn){
        *principal_valid = false;
        mvlist_free(list);
        return false;
    }

    bool complete = true;
    mvlist_t *alternative = list;
    while(alternative){
        if(alternative == selected){
            alternative = alternative->next;
            continue;
        }
        char defender_move[16];
        move_to_sfen(defender_move, alternative->mlist->move);
        if(!*first_variation) fprintf(stream, ",");
        fprintf(stream,
                "{\"branch_ply\":%u,\"defender_move\":\"%s\""
                ",\"proof_number\":%u,\"disproof_number\":%u"
                ",\"search_depth\":%u,\"surplus_piece\":%s"
                ",\"surplus_count\":%u,\"line\":[",
                S_COUNT(sdata)+1,
                defender_move,
                alternative->tdata.pn,
                alternative->tdata.dn,
                alternative->tdata.sh,
                alternative->inc ? "true" : "false",
                alternative->nouse2);

        bool first_move = true;
        json_emit_move(stream, alternative->mlist->move, &first_move);
        json_line_result_t result = {
            alternative->tdata.pn == 0,
            false,
            S_COUNT(sdata)+1,
            0,
            NULL,
            false
        };
        bool used_research = false;
        /* exclusivityの所有権:
         *  true  -> このresult.exclusivityはこの繰り返しでのみ有効。
         *           出力後にjson_free_exclusivityで解放する。
         *  false -> baseline構造体が所有しており、json_free_baselinesで
         *           後ほど一括解放されるため、ここでは解放しない。 */
        bool owns_exclusivity = true;
        if(alternative->tdata.pn == 0){
            sdata_t branch;
            memcpy(&branch, sdata, sizeof(sdata_t));
            sdata_move_forward(&branch, alternative->mlist->move);
            if(st_json_capture_baselines){
                json_baseline_line_t *baseline = json_capture_baseline(
                    S_COUNT(sdata)+1, alternative->mlist->move,
                    &branch, tbase, analyze_exclusivity);
                if(baseline && baseline->stream){
                    result = baseline->result;
                    first_move = baseline->first_move;
                    owns_exclusivity = false;
                    if(!json_copy_file(stream, baseline->stream))
                        result.complete = false;
                } else {
                    result = json_emit_line_or(
                        stream, &branch, tbase, &first_move, false,
                        analyze_exclusivity);
                }
            } else if(research_lines){
                json_baseline_line_t *baseline = json_find_baseline(
                    S_COUNT(sdata)+1, alternative->mlist->move);
                result = json_emit_researched_line_or(
                    stream, &branch, tbase, &first_move,
                    baseline,
                    &used_research, analyze_exclusivity);
                /* 採用されたのが再探索側(used_research)であれば所有権は
                 * このスコープにある。baseline側が採用された場合は
                 * baseline構造体が所有権を保持し続ける。baseline自体が
                 * 無い場合の通常線もこのスコープが所有する。 */
                owns_exclusivity =
                    used_research || !baseline || !baseline->stream;
            } else {
                result = json_emit_line_or(
                    stream, &branch, tbase, &first_move, false,
                    analyze_exclusivity);
            }
            fprintf(stream, "]");
            if(result.mate){
                fprintf(stream, ",\"terminal_ply\":%u", result.terminal_ply);
            }
            fprintf(stream, ",\"line_search\":\"%s\"",
                    used_research
                        ? "exact_attacker_minimax"
                        : "proof_tree_order");
        } else {
            fprintf(stream, "]");
            fprintf(stream, ",\"line_search\":\"proof_tree_order\"");
        }
        fprintf(stream, ",\"line_optimality\":\"%s\"",
                used_research
                    ? "proven_shortest_surplus_tiebreak"
                    : "unverified");
        fprintf(stream, ",\"complete\":%s", result.complete ? "true" : "false");
        if(analyze_exclusivity){
            fprintf(stream, ",\"attacker_move_exclusivity_complete\":%s",
                    result.exclusivity_complete ? "true" : "false");
            fprintf(stream, ",\"attacker_move_exclusivity\":");
            json_print_exclusivity(stream, result.exclusivity);
            if(owns_exclusivity) json_free_exclusivity(result.exclusivity);
        }
        fprintf(stream, "}");
        if(!result.complete || !result.mate) complete = false;
        *first_variation = false;
        alternative = alternative->next;
    }

    sdata_t next_state;
    memcpy(&next_state, sdata, sizeof(sdata_t));
    sdata_move_forward(&next_state, selected->mlist->move);
    if(!json_collect_variations_or(
        stream, &next_state, tbase, first_variation,
        principal, principal_length, research_lines, analyze_exclusivity,
        principal_valid)) complete = false;
    mvlist_free(list);
    return complete;
}

void tsume_debug                (const sdata_t   *sdata,
                                 tbase_t         *tbase)
{
    st_disp_flag = true;
    printf("=================詰手順確認モード開始=================\n");
    tsume_debug_or(sdata, tbase);
    printf("-----------------詰手順確認モード終了-----------------\n");
    return;
}

void generate_kif_file          (const char      *filename,
                                 const sdata_t   *sdata   ,
                                 tbase_t         *tbase  )
{
    FILE *fp;
    fp = fopen(filename, "w");
    if(!fp){
        //エラー処理
        if(!g_json_output){
            printf("info string I/O error:%s\n", strerror(errno));
            printf("info string %s\n",filename);
            printf("info string kif_file could't be created.\n");
        }
        return;
    }
    //コメント表記　プログラム名　バージョン
    fprintf(fp,
            "#KIF version=2.0 encoding=UTF-8\n"
            "# KIF形式棋譜ファイル\n"
            "# Generated by %s (%s)\n"
            ,PROGRAM_NAME, VERSION_INFO );
    //初期局面表記
    bod_fprintf(fp, sdata);
    //対局者
    fprintf(fp, "先手：\n後手：\n手数----指手---------消費時間--\n");
    //着手
    move_t dummy = {0,100};
    gen_kif_file_or(fp, sdata, tbase, dummy);
    
    fclose(fp);
    return;
}

void print_tbase                (const sdata_t   *sdata,
                                 turn_t              tn,
                                 tbase_t         *tbase)
{
    uint64_t address = HASH_FUNC(S_ZKEY(sdata), tbase);
    zfolder_t *zfolder = *(tbase->table+address);
    //局面表に同一盤面があるか
    while(zfolder){
        if(zfolder->zkey == S_ZKEY(sdata)) break;
        zfolder = zfolder->next;
    }
    //無い場合、その旨出力
    if(!zfolder){
        printf("No data exist in tbase.\n");
        return;
    }
    //同一盤面ありの場合、
    mcard_t *mcard = zfolder->mcard;
    unsigned int cmp_res;
    //mkey_t mkey = tn?S_GMKEY(sdata):S_SMKEY(sdata);
    mkey_t mkey;
    tn ? MKEY_COPY(mkey, S_GMKEY(sdata)):MKEY_COPY(mkey, S_SMKEY(sdata));
    memset(g_mcard, 0, sizeof(mcard_t *)*N_MCARD_TYPE);
    uint16_t tmp_pn = 1;
    while(mcard){
        cmp_res = MKEY_COMPARE(mkey, mcard->mkey);
        if     (cmp_res == MKEY_SUPER){
            //詰み
            if     (!mcard->tlist->tdata.pn){
                if(!g_mcard[SUPER_TSUMI]) g_mcard[SUPER_TSUMI] = mcard;
                else{
                    unsigned int res =
                    MKEY_COMPARE(g_mcard[SUPER_TSUMI]->mkey, mcard->mkey);
                    if(res == MKEY_SUPER) g_mcard[SUPER_TSUMI] = mcard;
                }
            }
            //不詰み
            else if(!mcard->tlist->tdata.dn){
                
            }
            //その他
            else                            {

            }
        }
        else if(cmp_res == MKEY_INFER){
            //詰み
            if     (!mcard->tlist->tdata.pn){
                
            }
            //不詰み
            else if(!mcard->tlist->tdata.dn){
                if(!g_mcard[INFER_FUDUMI]) g_mcard[INFER_FUDUMI] = mcard;
                else{
                    unsigned int res =
                    MKEY_COMPARE(g_mcard[INFER_FUDUMI]->mkey, mcard->mkey);
                    if(res == MKEY_INFER) g_mcard[INFER_FUDUMI] = mcard;
                }
            }
            //その他
            else                            {
                tlist_t *tlist = mcard->tlist;
                while(tlist){
                    tmp_pn = MAX(tmp_pn, tlist->tdata.pn);
                    tlist = tlist->next;
                }
                if(mcard->current){
                    tmp_pn = MAX(tmp_pn, mcard->cpn);
                }
            }
        }
        else if(cmp_res == MKEY_EQUAL){
            //詰み
            if     (   !mcard->tlist->tdata.pn ){
                if(!g_mcard[EQUAL_TSUMI]) g_mcard[EQUAL_TSUMI] = mcard;
            }
            //不詰み
            else if(   !mcard->tlist->tdata.dn ){
                if(!g_mcard[EQUAL_FUDUMI]) g_mcard[EQUAL_FUDUMI] = mcard;
            }
            //その他
            else                                {
                if(!g_mcard[EQUAL_UNKNOWN]) g_mcard[EQUAL_UNKNOWN] = mcard;
            }
        }
        mcard = mcard->next;
    }
    //データ内容表示
    if     (g_mcard[SUPER_TSUMI])  {
        printf("優越詰み %u手\n", g_mcard[SUPER_TSUMI]->tlist->tdata.sh);
        PRINT_MKEY(g_mcard[SUPER_TSUMI]->mkey);
        return;
    }
    else if(g_mcard[INFER_FUDUMI]) {
        printf("劣化不詰\n");
        return;
    }
    else if(g_mcard[EQUAL_TSUMI])  {
        printf("詰み %u手\n", g_mcard[EQUAL_TSUMI]->tlist->tdata.sh);
        return;
    }
    else if(g_mcard[EQUAL_FUDUMI]) {
        printf("不詰\n");
        return;
    }
    else if(g_mcard[EQUAL_UNKNOWN]){
        printf("不明\n");
        tlist_t *tlist = g_mcard[EQUAL_UNKNOWN]->tlist;
        while(tlist){
            printf("dp=%d ", tlist->dp);
            printf("pn=%d ", tlist->tdata.pn);
            printf("dn=%d ", tlist->tdata.dn);
            printf("sh=%d \n", tlist->tdata.sh);
            tlist = tlist->next;
        }
    }
    return;
}

/* -----------------
 スタティック関数実装部
 ----------------- */

void _tsume_print_or             (const sdata_t  *sdata,
                                 tbase_t         *tbase,
                                  unsigned int    flag  )
{
    MAKE_TREE_SET_PROTECT(sdata, S_TURN(sdata), tbase);
    
    //着手生成
    mvlist_t *list = generate_check(sdata, tbase);
    g_tsearchinf.nodes++;
    if(!list){
        printf("にて不詰\n");
        return;
    }
    
    //局面表を参照
    sdata_t sbuf;
    mvlist_t *tmp = list;
    while (tmp) {
        memcpy(&sbuf, sdata, sizeof(sdata_t));
        sdata_key_forward(&sbuf, tmp->mlist->move);
        make_tree_lookup(&sbuf, tmp, S_TURN(sdata), tbase);
        if(g_redundant){
            if(mtt_lookup(&sbuf, S_TURN(sdata), g_mtt))
                tmp->cu = 1;
        }
        tmp->length =
        g_distance[ENEMY_OU(sdata)][NEW_POS(tmp->mlist->move)];
        if(tmp->length > 1    &&
           tmp->tdata.pn == 1 &&
           tmp->tdata.dn == 1 &&
           tmp->tdata.sh == 0 &&
           MV_DROP(tmp->mlist->move)) tmp->tdata.pn = tmp->length;
        tmp = tmp->next;
    }
    
    //並べ替え
    list = sdata_mvlist_sort(list, sdata, proof_number_comp);
    //着手を表示
    if(flag & TP_ZKEY) printf("%u:0X%llX :", S_COUNT(sdata)+1, S_ZKEY(sdata));
    else               printf("%u:", S_COUNT(sdata)+1);
    tmp = list;
    while (tmp) {
        if(flag & TP_ALLMOVE){}
        else{
            if(tmp->tdata.pn) break;
        }
        MOVE_PRINTF(tmp->mlist->move, sdata);
        if(!g_redundant){
            printf("(%u %u %u %u) ",
                   tmp->tdata.pn,
                   tmp->tdata.sh,
                   tmp->inc,
                   tmp->nouse2);
        }
        tmp = tmp->next;
    }
    printf("\n");
    
    //エラーチェック
    if(list->cu){
        g_error = true;
        sprintf(g_error_location, "%s line %d", __FILE__, __LINE__);
        return;
    }
    if(!list->tdata.pn && !list->tdata.sh){
        printf("まで%u手詰め\n", S_COUNT(sdata)+1);
        return;
    }
    //着手を進める（再帰呼び出し)
    if(list && list->mlist){
        memcpy(&sbuf, sdata, sizeof(sdata_t));
        sdata_move_forward(&sbuf, list->mlist->move);
        _tsume_print_and(&sbuf, tbase, flag);
    }
    mvlist_free(list);
    return;
}

void _tsume_print_and           (const sdata_t  *sdata,
                                 tbase_t        *tbase,
                                 unsigned int     flag )
{
    if(S_COUNT(sdata)>=TSUME_MAX_DEPTH) return;

    //着手生成
    mvlist_t *list = generate_evasion(sdata, tbase);
    g_tsearchinf.nodes++;
    if(!list){
        unsigned int cnt = S_COUNT(sdata);
        printf("まで%u手詰め\n", cnt);
        return;
    }
    //局面表を参照
    mvlist_t *tmp = list, *tmp1;
    sdata_t sbuf;
    turn_t tn = TURN_FLIP(S_TURN(sdata));

    while (tmp) {
        memcpy(&sbuf, sdata, sizeof(sdata_t));
        sdata_key_forward(&sbuf, tmp->mlist->move);
        make_tree_lookup(&sbuf, tmp, tn, tbase);
        //合駒は全て展開しておく。
        if (tmp->mlist->next) {
            tmp1 = mvlist_alloc();
            tmp1->mlist = tmp->mlist->next;
            tmp->mlist->next = NULL;
            tmp1->next = tmp->next;
            tmp->next = tmp1;
        }
        tmp = tmp->next;
    }
    //並べ替え
    list = sdata_mvlist_sort(list, sdata, disproof_number_comp);
    
    //GCによるデータ消失対策
    tdata_t thdata = {INFINATE-1, INFINATE-1, TSUME_MAX_DEPTH};
    while (list->tdata.pn) {
        memcpy(&sbuf, sdata, sizeof(sdata_t));
        sdata_move_forward(&sbuf, list->mlist->move);
        bns_or(&sbuf, &thdata, list, tbase);
        list = sdata_mvlist_sort(list, sdata, disproof_number_comp);
    }
    mcard_t *current = MAKE_TREE_SET_CURRENT(sdata, tn, tbase);
    if(g_redundant)mtt_setup(sdata, tn, g_mtt);
    
    //着手を表示
    if(flag & TP_ZKEY) printf("%u:0X%llX :", S_COUNT(sdata)+1, S_ZKEY(sdata));
    else               printf("%u:", S_COUNT(sdata)+1);
    tmp = list;
    while (tmp) {
        MOVE_PRINTF(tmp->mlist->move, sdata);
        
        if(!g_redundant){
            printf("(%u %u %u %u) ",
                   tmp->tdata.pn,
                   tmp->tdata.sh,
                   tmp->inc,
                   tmp->nouse2);
        }
        tmp = tmp->next;
    }
    printf("\n");
    
    //着手を進める。（再帰呼び出し）
    if(list && list->mlist){
        memcpy(&sbuf, sdata, sizeof(sdata_t));
        sdata_move_forward(&sbuf, list->mlist->move);
        _tsume_print_or(&sbuf, tbase, flag);
    }
    if(current) current->current = 0;
    if(g_redundant)mtt_reset(sdata, tn, g_mtt);
    mvlist_free(list);
    return;
}

int _tsume_fprint_or            (FILE           *stream,
                                 const sdata_t  *sdata,
                                 tbase_t        *tbase,
                                 unsigned int    flag )
{
    int num = 0;
    MAKE_TREE_SET_PROTECT(sdata, S_TURN(sdata), tbase);
    
    //着手生成
    mvlist_t *list = generate_check(sdata, tbase);
    g_tsearchinf.nodes++;
    if(!list){
        num += fprintf(stream, "にて不詰\n");
        return num;
    }
    
    //局面表を参照
    sdata_t sbuf;
    mvlist_t *tmp = list;
    while (tmp) {
        memcpy(&sbuf, sdata, sizeof(sdata_t));
        sdata_key_forward(&sbuf, tmp->mlist->move);
        make_tree_lookup(&sbuf, tmp, S_TURN(sdata), tbase);
        if(g_redundant){
            if(mtt_lookup(&sbuf, S_TURN(sdata), g_mtt))
                tmp->cu = 1;
        }
        tmp->length =
        g_distance[ENEMY_OU(sdata)][NEW_POS(tmp->mlist->move)];
        if(tmp->length > 1    &&
           tmp->tdata.pn == 1 &&
           tmp->tdata.dn == 1 &&
           tmp->tdata.sh == 0 &&
           MV_DROP(tmp->mlist->move)) tmp->tdata.pn = tmp->length;
        tmp = tmp->next;
    }
    
    //並べ替え
    list = sdata_mvlist_sort(list, sdata, proof_number_comp);
    //着手を表示
    if(flag & TP_ZKEY)
        num +=
        fprintf(stream, "%u:0X%llX :", S_COUNT(sdata)+1, S_ZKEY(sdata));
    else
        num +=
        fprintf(stream, "%u:", S_COUNT(sdata)+1);
    
    tmp = list;
    while (tmp) {
        if(flag & TP_ALLMOVE){}
        else{
            if(tmp->tdata.pn) break;
        }
        num += move_fprintf(stream,tmp->mlist->move,sdata);
        
        if(!g_redundant){
            num += fprintf(stream, "(%u %u %u %u) ",
                           tmp->tdata.pn,
                           tmp->tdata.sh,
                           tmp->inc,
                           tmp->nouse2);
        }
        tmp = tmp->next;
    }
    num += fprintf(stream, "\n");
    
    //エラーチェック
    if(list->cu){
        g_error = true;
        sprintf(g_error_location, "%s line %d", __FILE__, __LINE__);
        return num;
    }
    
    //着手を進める（再帰呼び出し)
    if(list && list->mlist){
        memcpy(&sbuf, sdata, sizeof(sdata_t));
        sdata_move_forward(&sbuf, list->mlist->move);
        num += _tsume_fprint_and(stream, &sbuf, tbase, flag);
    }
    mvlist_free(list);
    
    return num;
}

int _tsume_fprint_and           (FILE           *stream,
                                 const sdata_t  *sdata,
                                 tbase_t        *tbase,
                                 unsigned int    flag )
{
    int num = 0;
    if(S_COUNT(sdata)>=TSUME_MAX_DEPTH) return num;
    
    //着手生成
    mvlist_t *list = generate_evasion(sdata, tbase);
    g_tsearchinf.nodes++;
    if(!list){
        unsigned int cnt = S_COUNT(sdata);
        num += fprintf(stream, "まで%u手詰め\n", cnt);
        return num;
    }
    
    //局面表を参照
    mvlist_t *tmp = list, *tmp1;
    sdata_t sbuf;
    turn_t tn = TURN_FLIP(S_TURN(sdata));
    
    while (tmp) {
        memcpy(&sbuf, sdata, sizeof(sdata_t));
        sdata_key_forward(&sbuf, tmp->mlist->move);
        make_tree_lookup(&sbuf, tmp, tn, tbase);
        //合駒は全て展開しておく。
        if (tmp->mlist->next) {
            tmp1 = mvlist_alloc();
            tmp1->mlist = tmp->mlist->next;
            tmp->mlist->next = NULL;
            tmp1->next = tmp->next;
            tmp->next = tmp1;
        }
        tmp = tmp->next;
    }
    //並べ替え
    list = sdata_mvlist_sort(list, sdata, disproof_number_comp);
    //GCによるデータ消失対策
    tdata_t thdata = {INFINATE-1, INFINATE-1, TSUME_MAX_DEPTH};
    while (list->tdata.pn) {
        memcpy(&sbuf, sdata, sizeof(sdata_t));
        sdata_move_forward(&sbuf, list->mlist->move);
        bns_or(&sbuf, &thdata, list, tbase);
        list = sdata_mvlist_sort(list, sdata, disproof_number_comp);
    }
    mcard_t *current = MAKE_TREE_SET_CURRENT(sdata, tn, tbase);
    if(g_redundant)mtt_setup(sdata, tn, g_mtt);
    
    //着手を表示
    if(flag & TP_ZKEY)
        num +=
        fprintf(stream, "%u:0X%llX :", S_COUNT(sdata)+1, S_ZKEY(sdata));
    else
        num +=
        fprintf(stream, "%u:", S_COUNT(sdata)+1);
    tmp = list;
    while (tmp) {
        num += move_fprintf(stream,tmp->mlist->move,sdata);
        if(!g_redundant){
            num += fprintf(stream, "(%u %u %u %u) ",
                           tmp->tdata.pn,
                           tmp->tdata.sh,
                           tmp->inc,
                           tmp->nouse2);
        }
        tmp = tmp->next;
    }
    num += fprintf(stream, "\n");
    
    //着手を進める。（再帰呼び出し）
    if(list && list->mlist){
        memcpy(&sbuf, sdata, sizeof(sdata_t));
        sdata_move_forward(&sbuf, list->mlist->move);
        num += _tsume_fprint_or(stream, &sbuf, tbase, flag);
    }
    if(current) current->current = 0;
    if(g_redundant)mtt_reset(sdata, tn, g_mtt);
    mvlist_free(list);
    return num;
}

void tsume_debug_or              (const sdata_t   *sdata,
                                  tbase_t         *tbase)
{
    //着手生成
    mvlist_t *list = generate_check(sdata, tbase);
    g_tsearchinf.nodes++;
    
    if(!list){
        printf("--------------------------------------------------\n");
        SDATA_PRINTF(sdata, PR_BOARD);
        printf("%u手目\n", S_COUNT(sdata)+1);
        printf("ID:合法手(pn,dn,詰手数,駒余り,枚数)\n"
               "-------------------------------\n");
        printf("にて不詰 (b(back)/q(quit)\n");
        //戻りor終了選択
        char str[16];
        memset(str, 0, sizeof(str));
        while(true){
            fgets(str, sizeof(str)-1, stdin);
            if(mblen(str, 16)!=1){
                printf("文字コードが間違っています。再度入力してください\n");
                memset(str, 0, sizeof(str));
            }
            else if(!memcmp(str, "b", sizeof(char)*1)) break;
            else if(!memcmp(str, "q", sizeof(char)*1)){
                st_disp_flag = false;
                break;
            }
            else{
                printf("入力が間違っています。再度入力してください\n");
            }
        }
        return;
    }
    //局面表を参照
    sdata_t sbuf;
    mvlist_t *tmp = list;
    while (tmp) {
        memcpy(&sbuf, sdata, sizeof(sdata_t));
        sdata_key_forward(&sbuf, tmp->mlist->move);
        make_tree_lookup(&sbuf, tmp, S_TURN(sdata), tbase);
        //千日手判定
        if(g_redundant){
            if(mtt_lookup(&sbuf, S_TURN(sdata), g_mtt))
                tmp->cu = 1;
        }
        tmp->length =
        g_distance[ENEMY_OU(sdata)][NEW_POS(tmp->mlist->move)];
        
        if(tmp->length > 1    &&
           tmp->tdata.pn == 1 &&
           tmp->tdata.dn == 1 &&
           tmp->tdata.sh == 0 &&
           MV_DROP(tmp->mlist->move)) tmp->tdata.pn = tmp->length;
        
        tmp = tmp->next;
    }
    //並べ替え
    list = sdata_mvlist_sort(list, sdata, proof_number_comp);
    int num;
    char str[16];
    unsigned int select;
    while(true){
        printf("--------------------------------------------------\n");
        //局面表示
        SDATA_PRINTF(sdata, PR_BOARD);
        //着手表示
        printf("%u手目\n", S_COUNT(sdata)+1);
        if(g_redundant){
            printf("ID:合法手(pn,dn)\n"
                   "-------------------------------\n");
        }else{
            printf("ID:合法手(pn,dn,詰手数,駒余り,枚数)\n"
                   "-------------------------------\n");
        }
        tmp = list;
        num = 0;
        while (tmp) {
            printf("%2d: ", num); num++;
            MOVE_PRINTF(tmp->mlist->move, sdata);
            if(!g_redundant){
                printf("(%u %u %u %u %u) \n",
                       tmp->tdata.pn,
                       tmp->tdata.dn,
                       tmp->tdata.sh,
                       tmp->inc,
                       tmp->nouse2);
            } else{
                printf("(%u %u) \n",
                       tmp->tdata.pn,
                       tmp->tdata.dn);
            }
            tmp = tmp->next;
        }
        //着手＆終了選択
        if(S_COUNT(sdata))
            printf("着手を選択してください（数字/b(back)/q(quit))\n");
        else
            printf("着手を選択してください（数字/q(quit))\n");
        fgets(str, sizeof(str)-1, stdin);
        if(mblen(str, 16)!=1){
            printf("文字コードが間違っています。再度入力してください\n");
            memset(str, 0, sizeof(str));
        }
        else if(S_COUNT(sdata) && !memcmp(str, "b", sizeof(char)*1)) break;
        else if(!memcmp(str, "q", sizeof(char)*1)){
            st_disp_flag = false;
            break;
        }
        else if(str[0]<'0' || str[0]>'9'){
            printf("入力が間違っています。再度入力してください\n");
        }
        else{
            //エラーチェック
            if(list->cu){
                g_error = true;
                sprintf(g_error_location, "%s line %d", __FILE__, __LINE__);
                return;
            }
            select = atoi(str);
            tmp = mvlist_nth(list, select);
            //着手を進める（再帰呼び出し)
            if(tmp){
                memcpy(&sbuf, sdata, sizeof(sdata_t));
                sdata_move_forward(&sbuf, tmp->mlist->move);
                tsume_debug_and(&sbuf, tbase);
                if(!st_disp_flag) break;
            }
        }
    }
    mvlist_free(list);
    return;
}

void tsume_debug_and             (const sdata_t   *sdata,
                                  tbase_t         *tbase)
{
    //着手生成
    mvlist_t *list = generate_evasion(sdata, tbase);
    g_tsearchinf.nodes++;
    if(!list){
        unsigned int cnt = S_COUNT(sdata);
        printf("--------------------------------------------------\n");
        SDATA_PRINTF(sdata, PR_BOARD);
        printf("%u手目\n", S_COUNT(sdata)+1);
        printf("ID:合法手(pn,dn,詰手数,駒余り,枚数)\n"
               "-------------------------------\n");
        printf("まで%u手詰め (b(back)/q(quit))\n", cnt);
        //戻りor終了選択
        char str[16];
        memset(str, 0, sizeof(str));
        while(true){
            fgets(str, sizeof(str)-1, stdin);
            if(mblen(str, 16)!=1){
                printf("文字コードが間違っています。再度入力してください\n");
                memset(str, 0, sizeof(str));
            }
            else if(!memcmp(str, "b", sizeof(char)*1)) break;
            else if(!memcmp(str, "q", sizeof(char)*1)){
                st_disp_flag = false;
                break;
            }
            else{
                printf("入力が間違っています。再度入力してください\n");
            }
        }
        return;
    }
    //局面表を参照
    mvlist_t *tmp = list, *tmp1;
    sdata_t sbuf;
    turn_t tn = TURN_FLIP(S_TURN(sdata));
    
    while (tmp) {
        memcpy(&sbuf, sdata, sizeof(sdata_t));
        sdata_key_forward(&sbuf, tmp->mlist->move);
        make_tree_lookup(&sbuf, tmp, tn, tbase);
        //合駒は全て展開しておく。
        if (tmp->mlist->next) {
            tmp1 = mvlist_alloc();
            tmp1->mlist = tmp->mlist->next;
            tmp->mlist->next = NULL;
            tmp1->next = tmp->next;
            tmp->next = tmp1;
        }
        tmp = tmp->next;
    }
    //並べ替え
    list = sdata_mvlist_sort(list, sdata, disproof_number_comp);
    
    //GCによるデータ消失対策
    tdata_t thdata = {INFINATE-1, INFINATE-1, TSUME_MAX_DEPTH};
    while (list->tdata.pn) {
        memcpy(&sbuf, sdata, sizeof(sdata_t));
        sdata_move_forward(&sbuf, list->mlist->move);
        bns_or(&sbuf, &thdata, list, tbase);
        list = sdata_mvlist_sort(list, sdata, disproof_number_comp);
    }
    
    mcard_t *current = MAKE_TREE_SET_CURRENT(sdata, tn, tbase);
    if(g_redundant)mtt_setup(sdata, tn, g_mtt);
    //着手の表示
    int num;
    char str[16];
    unsigned int select;
    while(true){
        printf("--------------------------------------------------\n");
        //局面の表示
        SDATA_PRINTF(sdata, PR_BOARD);
        //着手表示
        printf("%u手目\n", S_COUNT(sdata)+1);
        if(g_redundant){
            printf("ID:合法手(pn,dn)\n"
                   "-------------------------------\n");
        } else{
            printf("ID:合法手(pn,dn,詰手数,駒余り,枚数)\n"
                   "-------------------------------\n");
        }
        tmp = list;
        num = 0;
        while (tmp) {
            printf("%2d: ", num); num++;
            MOVE_PRINTF(tmp->mlist->move, sdata);
            if(!g_redundant){
                printf("(%u %u %u %u %u) \n",
                       tmp->tdata.pn,
                       tmp->tdata.dn,
                       tmp->tdata.sh,
                       tmp->inc,
                       tmp->nouse2);
            } else{
                printf("(%u %u) \n",
                       tmp->tdata.pn,
                       tmp->tdata.dn);
            }
            tmp = tmp->next;
        }
        //着手&終了選択
        printf("着手を選択してください（数字/b(back)/q(quit))\n");
        fgets(str, sizeof(str)-1, stdin);
        if(mblen(str, 16)!=1){
            printf("文字コードが間違っています。再度入力してください\n");
            memset(str, 0, sizeof(str));
        }
        else if(!memcmp(str, "b", sizeof(char)*1)) break;
        else if(!memcmp(str, "q", sizeof(char)*1)){
            st_disp_flag = false;
            break;
        }
        else if(str[0]<'0' || str[0]>'9'){
            printf("入力が間違っています。再度入力してください\n");
        }
        else{
            select = atoi(str);
            tmp = mvlist_nth(list, select);
            //着手を進める（再帰呼び出し)
            if(tmp){
                memcpy(&sbuf, sdata, sizeof(sdata_t));
                sdata_move_forward(&sbuf, tmp->mlist->move);
                tsume_debug_or(&sbuf, tbase);
                if(!st_disp_flag) break;
            }
        }
    }
    if(current) current->current = 0;
    if(g_redundant)mtt_reset(sdata, tn, g_mtt);
    mvlist_free(list);
    return;
}

void gen_kif_file_or             (FILE *restrict  stream,
                                  const sdata_t   *sdata,
                                  tbase_t         *tbase,
                                  move_t           prev  )
{
    MAKE_TREE_SET_PROTECT(sdata, S_TURN(sdata), tbase);
    
    //着手生成
    mvlist_t *list = generate_check(sdata, tbase);
    g_tsearchinf.nodes++;
    if(!list){
        printf("にて不詰\n");
        return;
    }
    
    //局面表を参照
    sdata_t sbuf;
    mvlist_t *tmp = list;
    while (tmp) {
        memcpy(&sbuf, sdata, sizeof(sdata_t));
        sdata_key_forward(&sbuf, tmp->mlist->move);
        make_tree_lookup(&sbuf, tmp, S_TURN(sdata), tbase);
        tmp->length =
        g_distance[ENEMY_OU(sdata)][NEW_POS(tmp->mlist->move)];
        
        if(tmp->length > 1    &&
           tmp->tdata.pn == 1 &&
           tmp->tdata.dn == 1 &&
           tmp->tdata.sh == 0 &&
           MV_DROP(tmp->mlist->move)) tmp->tdata.pn = tmp->length;
        
        tmp = tmp->next;
    }
    //並べ替え
    list = sdata_mvlist_sort(list, sdata, proof_number_comp);
    
    //先頭着手を表示
    move_t move = list->mlist->move;
    clock_t elapsed = g_tsearchinf.elapsed;
    kif_move_fprintf(stream, sdata, move, prev, elapsed, elapsed);
    
    //着手を進める（再帰呼び出し)
    if(list && list->mlist){
        memcpy(&sbuf, sdata, sizeof(sdata_t));
        sdata_move_forward(&sbuf, list->mlist->move);
        gen_kif_file_and(stream, &sbuf, tbase, move);
    }
    mvlist_free(list);
    return;
}

void gen_kif_file_and            (FILE *restrict  stream,
                                  const sdata_t   *sdata,
                                  tbase_t         *tbase,
                                  move_t           prev  )
{
    if(S_COUNT(sdata)>=TSUME_MAX_DEPTH) return;
    turn_t tn = TURN_FLIP(S_TURN(sdata));
    //着手生成
    mvlist_t *list = generate_evasion(sdata, tbase);
    g_tsearchinf.nodes++;
    if(!list){
        fprintf(stream, "%4u 詰み", S_COUNT(sdata)+1);
        return;
    }
    //局面表を参照
    mvlist_t *tmp = list, *tmp1;
    sdata_t sbuf;

    while (tmp) {
        memcpy(&sbuf, sdata, sizeof(sdata_t));
        sdata_key_forward(&sbuf, tmp->mlist->move);
        make_tree_lookup(&sbuf, tmp, tn, tbase);
        //合駒は全て展開しておく。
        if (tmp->mlist->next) {
            tmp1 = mvlist_alloc();
            tmp1->mlist = tmp->mlist->next;
            tmp->mlist->next = NULL;
            tmp1->next = tmp->next;
            tmp->next = tmp1;
        }
        tmp = tmp->next;
    }
    //並べ替え
    list = sdata_mvlist_sort(list, sdata, disproof_number_comp);
    
    //先頭着手を表示
    move_t move = list->mlist->move;
    kif_move_fprintf(stream, sdata, move, prev, 0, 0);
    
    //着手を進める（再帰呼び出し)
    if(list && list->mlist){
        memcpy(&sbuf, sdata, sizeof(sdata_t));
        sdata_move_forward(&sbuf, list->mlist->move);
        gen_kif_file_or(stream, &sbuf, tbase, move);
    }
    mvlist_free(list);
    return;
}
