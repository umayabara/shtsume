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

/* 一局面での王手候補数は将棋の合法手数の範囲で十分な余裕を持たせる */
#define JSON_EXCLUSIVITY_MAX_ALT   200

/*
 * 排他性判定のための追加探索において、選択済み手順の詰み手数
 * (list->tdata.sh)に対して許容する深さの余裕。反証(不詰の証明)は
 * 全分岐の消尽を要し証明よりはるかに高コストなため、TSUME_MAX_DEPTH
 * まで無制限に許すと現実的な時間で終わらない候補が生じ得る。ここで
 * 有限のマージンを設けることで、選択手より大幅に長い別詰みの可能性は
 * "unverified" として扱われる(不詰とは断定しない)。
 */
#define JSON_EXCLUSIVITY_DEPTH_MARGIN   20

typedef struct json_exclusivity_entry {
    unsigned int ply;                 /* 攻方着手の手数(1始まり)        */
    move_t       move;                /* 実際に選択された着手           */
    json_exclusivity_status_t status;
    unsigned int alternative_count;   /* 選択手以外の合法な王手候補数    */
    move_t       proven_mating[JSON_EXCLUSIVITY_MAX_ALT];
    unsigned int proven_mating_count; /* 詰みと証明された他候補の数     */
    struct json_exclusivity_entry *next;
} json_exclusivity_entry_t;

typedef struct {
    bool complete;
    bool mate;
    unsigned int terminal_ply;
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
static tbase_t *st_json_exclusivity_tbase;

static mvlist_t* json_prepare_or(const sdata_t *sdata, tbase_t *tbase);
static mvlist_t* json_optimize_or(
    mvlist_t *list, const sdata_t *sdata, tbase_t *tbase);
static mvlist_t* json_prepare_and(const sdata_t *sdata, tbase_t *tbase);
static json_line_result_t json_emit_line_or(
    FILE *stream, const sdata_t *sdata, tbase_t *tbase, bool *first_move,
    bool research_lines, bool analyze_exclusivity);
static json_line_result_t json_emit_line_and(
    FILE *stream, const sdata_t *sdata, tbase_t *tbase, bool *first_move,
    bool research_lines, bool analyze_exclusivity);
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

bool tsume_json_variations_fprint(
    FILE *stream, const sdata_t *sdata, tbase_t *tbase,
    const move_t *principal, unsigned int principal_length,
    bool research_lines, bool analyze_exclusivity,
    bool *principal_valid)
{
    bool first_variation = true;
    *principal_valid = true;
    st_json_baselines = NULL;
    st_json_exclusivity_tbase = analyze_exclusivity
        ? create_tbase(
            MIN(tbase->sz_elm, 16 * MCARDS_PER_MBYTE - 1))
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
    entry->alternative_count = 0;
    entry->proven_mating_count = 0;
    entry->next = NULL;

    bool any_unresolved = false;
    unsigned int depth_limit = list->tdata.sh + JSON_EXCLUSIVITY_DEPTH_MARGIN;
    if(depth_limit > TSUME_MAX_DEPTH || depth_limit < list->tdata.sh)
        depth_limit = TSUME_MAX_DEPTH;
    tdata_t threshold = {INFINATE-1, INFINATE-1, depth_limit};
    mvlist_t *candidate = list;
    while(candidate){
        if(candidate->mlist->move.prev_pos != selected_move.prev_pos ||
           candidate->mlist->move.new_pos  != selected_move.new_pos){
            entry->alternative_count++;
            tdata_t candidate_result = candidate->tdata;
            if(candidate_result.pn != 0 && candidate_result.dn != 0 &&
               st_json_exclusivity_tbase){
                initialize_tbase(st_json_exclusivity_tbase);
                mvlist_t probe = *candidate;
                probe.next = NULL;
                probe.tdata = (tdata_t){1, 1, 0};
                probe.hinc = 0;
                probe.inc = 0;
                probe.nouse = 0;
                probe.nouse2 = 0;
                sdata_t child;
                memcpy(&child, sdata, sizeof(sdata_t));
                sdata_move_forward(&child, candidate->mlist->move);
                bns_and_isolated(
                    &child, &threshold, &probe,
                    st_json_exclusivity_tbase);
                candidate_result = probe.tdata;
            }
            if(candidate_result.pn == 0){
                if(entry->proven_mating_count < JSON_EXCLUSIVITY_MAX_ALT){
                    entry->proven_mating[entry->proven_mating_count] =
                        candidate->mlist->move;
                }
                entry->proven_mating_count++;
            } else if(candidate_result.dn != 0){
                /* pn!=0 かつ dn!=0: 追加探索でも未解決のまま */
                any_unresolved = true;
            }
            /* dn==0 の場合は不詰と証明済みのため、何もしない */
        }
        candidate = candidate->next;
    }
    if(entry->proven_mating_count > 0)
        entry->status = JSON_EXCLUSIVITY_NONEXCLUSIVE;
    else if(any_unresolved)
        entry->status = JSON_EXCLUSIVITY_UNVERIFIED;
    else
        entry->status = JSON_EXCLUSIVITY_EXCLUSIVE;
    return entry;
}

static void json_free_exclusivity(json_exclusivity_entry_t *list)
{
    while(list){
        json_exclusivity_entry_t *next = list->next;
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
    fprintf(stream, ",\"attacker_move_exclusivity\":[");
    bool first_entry = true;
    while(list){
        char move_string[16];
        move_to_sfen(move_string, list->move);
        if(!first_entry) fprintf(stream, ",");
        fprintf(stream,
                "{\"ply\":%u,\"move\":\"%s\",\"status\":\"%s\""
                ",\"alternative_count\":%u"
                ",\"proven_mating_alternatives\":[",
                list->ply, move_string,
                json_exclusivity_status_string(list->status),
                list->alternative_count);
        bool first_alt = true;
        unsigned int shown = MIN(list->proven_mating_count,
                                  JSON_EXCLUSIVITY_MAX_ALT);
        for(unsigned int i=0; i<shown; i++){
            json_emit_move(stream, list->proven_mating[i], &first_alt);
        }
        fprintf(stream, "]}");
        first_entry = false;
        list = list->next;
    }
    fprintf(stream, "]");
}

static json_line_result_t json_emit_line_or(
    FILE *stream, const sdata_t *sdata, tbase_t *tbase, bool *first_move,
    bool research_lines, bool analyze_exclusivity)
{
    json_line_result_t result = {true, false, S_COUNT(sdata), NULL, true};
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
    json_line_result_t result = {true, false, S_COUNT(sdata), NULL, true};
    if(S_COUNT(sdata) >= TSUME_MAX_DEPTH){
        result.complete = false;
        result.exclusivity_complete = false;
        return result;
    }

    mvlist_t *list = json_prepare_and(sdata, tbase);
    if(!list){
        result.mate = true;
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
        false, false, branch_ply, NULL, false
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
    json_line_result_t researched = json_emit_line_or(
        research_stream, sdata, tbase, &research_first, true,
        analyze_exclusivity);

    bool choose_research =
        researched.complete && researched.mate &&
        (!baseline->result.complete || !baseline->result.mate ||
         researched.terminal_ply <= baseline->result.terminal_ply);
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
            NULL,
            false
        };
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
            bool used_research = false;
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
                result = json_emit_researched_line_or(
                    stream, &branch, tbase, &first_move,
                    json_find_baseline(
                        S_COUNT(sdata)+1, alternative->mlist->move),
                    &used_research, analyze_exclusivity);
                /* 採用されたのが再探索側(used_research)であれば所有権は
                 * このスコープにある。baseline側が採用された場合は
                 * baseline構造体が所有権を保持し続ける。 */
                owns_exclusivity = used_research;
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
                        ? "bounded_attacker_candidates"
                        : "proof_tree_order");
        } else {
            fprintf(stream, "]");
            fprintf(stream, ",\"line_search\":\"proof_tree_order\"");
        }
        fprintf(stream, ",\"line_optimality\":\"unverified\"");
        fprintf(stream, ",\"complete\":%s", result.complete ? "true" : "false");
        if(analyze_exclusivity){
            fprintf(stream, ",\"attacker_move_exclusivity_complete\":%s",
                    result.exclusivity_complete ? "true" : "false");
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
