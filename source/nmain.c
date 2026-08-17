//
//  main.c
//  shtsume
//
//  Created by Hkijin on 2022/02/02.
//

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <getopt.h>
#include <unistd.h>
#include "shogi.h"
#include "usi.h"
#include "shtsume.h"
#include "ndtools.h"
//#include "ntest.h"

/* --------------
 グローバル変数
 -------------- */
bool        g_commandline;       /* true: commandline  false:  usi-mode  */
bool        g_json_output;

/* --------------
 スタティック変数
 -------------- */

static bool st_display;
static bool st_json;
static char *st_principal_string;
static const struct option longopts[] =
{
 //サポートオプション
     {"help",    no_argument,        NULL,   'h'},
     {"version", no_argument,        NULL,   'v'},
 //探索オプション
     {"kifu",    no_argument,        NULL,   'k'},  //g_out_lvkif
     {"log",     no_argument,        NULL,   'g'},  //g_summary
     {"display", no_argument,        NULL,   'd'},  //st_display
     {"json",    no_argument,        NULL,   'J'},  //st_json
     {"principal", required_argument, NULL,  'P'},  //st_principal_string
     {"yomi",    no_argument,        NULL,   'y'},  //g_disp_search
     {"all",     no_argument,        NULL,   'a'},  //g_smode
 //値指定
     {"minpn",   required_argument,  NULL,   'n'},  //g_mt_min_pn
     {"memory",  required_argument,  NULL,   'm'},  //g_usi_hash
     {"level",   required_argument,  NULL,   'l'},  //g_search_level
     {"limit",   required_argument,  NULL,   'i'},  //g_time_limit
     {"ydep",    required_argument,  NULL,   'j'},  //g_pv_length
     {"yint",    required_argument,  NULL,   't'}   //g_info_interval
};

static void print_version (void);

/* -----------
 実装
 ------------ */
 
int main(int argc, char * const argv[]) {
    //argc,argvの処理
    int optc;
    g_info_interval = 5;
    g_pv_length     = PV_LENGTH_DEFAULT;
    while((optc = getopt_long(argc, argv, "hvkgdJyaP:n:m:l:i:j:t:",
                              longopts, NULL))!= -1)
        switch(optc){
            case 'h':
                print_help();
                exit(EXIT_SUCCESS);
                break;
            case 'v':
                print_version();
                exit(EXIT_SUCCESS);
                break;
                
            case 'k': g_out_lvkif = true;   break;
            case 'g': g_summary = true;     break;
            case 'd': st_display = true;    break;
            case 'J': st_json = true;       break;
            case 'P': st_principal_string = optarg; break;
            case 'y': g_disp_search = true; break;
            case 'a': g_smode = (TP_NONE|TP_ALLMOVE); break;
                
            case 'n':
                g_mt_min_pn = atoi(optarg);
                g_mt_min_pn = MAX(LEAF_PN_MIN,g_mt_min_pn);
                g_mt_min_pn = MIN(LEAF_PN_MAX,g_mt_min_pn);
                break;
            case 'm':
                g_usi_hash  = atoi(optarg);
                g_usi_hash  = MAX(TBASE_SIZE_MIN,g_usi_hash);
                g_usi_hash  = MIN(TBASE_SIZE_MAX,g_usi_hash);
                break;
            case 'l': g_search_level = atoi(optarg);   break;
            case 'i':
                g_time_limit = atoi(optarg);
                g_time_limit *= 1000;
                break;
            case 'j':
                g_pv_length = atoi(optarg);
                g_pv_length = MAX(PV_LENGTH_MIN,g_pv_length);
                g_pv_length = MIN(PV_LENGTH_MAX,g_pv_length);
                break;
            case 't':
                g_info_interval = atoi(optarg);
                g_info_interval = MAX(PV_INTERVAL_MIN,g_info_interval);
                g_info_interval = MIN(PV_INTERVAL_MAX,g_info_interval);
                break;
        };
    // オプション、引数なしの場合、usiエンジンとして起動
    if(argc == 1){
        g_commandline = false;
        g_info_interval = 1;
        g_pv_length     = 20;
        //基本ライブラリの初期化処理
        create_seed();                        //zkey
        init_distance();                      //g_distance
        init_bpos();                          //bitboard
        init_effect();                        //effect
        srand((unsigned)time(NULL));
        
        usi_main();
        
        return 0;
    }
    // sfen_stringが欠落している場合、helpを表示して終了。
    else if (argc != optind+1){
        print_help();
        exit(EXIT_SUCCESS);
    }
    // コマンドラインアプリとして起動
    else{
        g_commandline   = true;
        g_json_output   = st_json;
        if(g_json_output){
            g_disp_search = false;
            g_out_lvkif = false;
            g_summary = false;
            st_display = false;
        }
        //char p_string[128];
        strncpy(g_sfen_pos_str, argv[optind], strlen(argv[optind]));
        char *home = getenv("HOME");
        if(home) strncpy(g_user_path, home, strlen(home));
        else     home = getcwd(g_user_path, sizeof(g_user_path));
        
        //局面デバッグ用
        g_debug_enable = false;              //デバッグの場合true
        g_test_mkey.ky = 1;                  //調べたい局面の持駒キーを入力
        g_test_zkey = 0X64E474062DD95AE6;    //調べたい局面の盤面キーを入力
        strncpy(g_logfile_path,g_user_path,sizeof(g_logfile_path));
        create_debug_filename();
        
        //基本ライブラリの初期化処理
        create_seed();                        //zkey
        init_distance();                      //g_distance
        init_bpos();                          //bitboard
        init_effect();                        //effect
        srand((unsigned)time(NULL));

        //詰将棋条件の表示
        ssdata_t ssdata;
        sfen_to_ssdata(g_sfen_pos_str, &ssdata);
        initialize_sdata(&g_sdata, &ssdata);
        move_t requested_principal[TSUME_MAX_DEPTH];
        unsigned int requested_principal_length = 0;
        if(st_principal_string){
            char principal_buffer[TSUME_MAX_DEPTH*7];
            strncpy(principal_buffer, st_principal_string,
                    sizeof(principal_buffer)-1);
            principal_buffer[sizeof(principal_buffer)-1] = '\0';
            char *token = strtok(principal_buffer, " ");
            while(token &&
                  requested_principal_length < TSUME_MAX_DEPTH){
                char move_buffer[8];
                snprintf(move_buffer, sizeof(move_buffer), "%s ", token);
                sfen_to_move(
                    &requested_principal[requested_principal_length],
                    move_buffer);
                requested_principal_length++;
                token = strtok(NULL, " ");
            }
        }
        if(!st_json){
            printf("-----局面図-----\n");
            SDATA_PRINTF(&g_sdata, PR_BOARD);
        }
        
        //局面の合法性check
        int err = is_sdata_illegal(&g_sdata);
        if(err){
            if(st_json){
                const char *message = NULL;
                switch(err){
                    case CHECKED:
                        message = "すでに相手玉に王手がかかった局面です。";
                        break;
                    case NIFU:
                        message = "二歩のある局面です。";
                        break;
                    case ILL_POS:
                        message = "行きどころの無い駒があります。";
                        break;
                }
                printf("{\"status\":\"error\",\"error_code\":%d,\"message\":\"%s\"}\n",
                       err, message ? message : "局面エラー");
            } else {
                printf("局面エラー: ");
                switch(err){
                    case CHECKED:
                        printf("すでに相手玉に王手がかかった局面です。\n");
                        break;
                    case NIFU:
                        printf("二歩のある局面です。\n");
                        break;
                    case ILL_POS:
                        printf("行きどころの無い駒があります。\n");
                        break;
                }
            }
            return 1;
        }
        
        //tbaseの作成
        uint64_t size = g_usi_hash*MCARDS_PER_MBYTE-1;
        g_tbase = create_tbase(size);
        g_mtt = create_mtt(MTT_SIZE);
        if(!g_time_limit) g_time_limit = TM_INFINATE;

        if(!st_json){
            //探索条件の表示
            printf("---------探索条件---------\n");
            printf("局面表 %llu(MByte)\n",g_usi_hash);
            printf("探索レベル %u\n", g_search_level);
            if(g_time_limit<0) printf("探索時間 制限なし\n");
            else printf("探索時間 %d(sec)\n",g_time_limit/1000);
            printf("読み筋表示 ");
            if(g_disp_search) printf("あり\n");
            else              printf("なし\n");
            if(g_disp_search){
                printf("表示間隔 %ld(sec)\n", g_info_interval);
                printf("表示深さ %d\n", g_pv_length);
            }
            printf("棋譜出力 ");
            if(g_out_lvkif)   printf("あり\n");
            else              printf("なし\n");
            printf("ログ出力 ");
            if(g_summary)     printf("あり\n");
            else              printf("なし\n");
            if(g_out_lvkif||g_summary){
                printf("出力先: %s\n", g_user_path);
            }
            printf("手順確認 ");
            if(st_display)     printf("あり\n");
            else               printf("なし\n");
            printf("-------------------------\n");
        }
        //詰探索
        clock_t start = clock();
        tdata_t tdata;
        
        bn_search(&g_sdata, &tdata, g_tbase);
        
        clock_t finish = clock();
        
        if(st_json){
            int num = 0;
            char mvstr[16];
            const char *status;
            if(!tdata.pn)      status = "mate";
            else if(!tdata.dn) status = "no_mate";
            else               status = "unknown";
            printf("{");
            printf("\"status\":\"%s\"", status);
            printf(",\"redundant\":%s", g_redundant ? "true" : "false");
            printf(",\"futile_interposition_pruning\":\"builtin\"");
            printf(",\"variation_collection\":\"single_proof_tree\"");
            printf(",\"variation_line_optimality\":\"unverified\"");
            printf(",\"search_config\":{\"memory_mb\":%llu"
                   ",\"min_proof_number\":%u,\"level\":%u}",
                   g_usi_hash, g_mt_min_pn, g_search_level);
            if(!tdata.pn){
                printf(",\"mate_in\":%u", tdata.sh);
            }
            else{
                printf(",\"proof_number\":%u", tdata.pn);
                printf(",\"disproof_number\":%u", tdata.dn);
                printf(",\"search_depth\":%u", tdata.sh);
            }
            printf(",\"elapsed_sec\":%lu", (finish-start)/CLOCKS_PER_SEC);
            printf(",\"nodes\":%llu", g_tsearchinf.nodes);
            printf(",\"tree_nodes\":%llu", g_tbase->pr_num);
            printf(",\"table_entries\":%llu", g_tbase->num);
            printf(",\"principal_variation\":[");
            unsigned int output_principal_length =
                requested_principal_length ? requested_principal_length
                                           : TSUME_MAX_DEPTH;
            for(unsigned int i=0; i<output_principal_length; i++){
                move_t mv = requested_principal_length
                    ? requested_principal[i]
                    : g_tsearchinf.mvinf[i].move;
                if(mv.prev_pos == 0 && mv.new_pos == 0) break;
                if(MV_TORYO(mv)) break;
                if(num++) printf(",");
                move_to_sfen(mvstr, mv);
                printf("\"%s\"", mvstr);
            }
            printf("]");
            if(!tdata.pn){
                printf(",\"variations\":");
                bool principal_valid;
                bool variations_complete =
                    tsume_json_variations_fprint(
                        stdout, &g_sdata, g_tbase,
                        requested_principal_length ? requested_principal : NULL,
                        requested_principal_length, &principal_valid);
                printf(",\"principal_variation_valid\":%s",
                       principal_valid ? "true" : "false");
                printf(",\"variations_complete\":%s",
                       variations_complete ? "true" : "false");
            }
            printf("}\n");
        } else if(!tdata.pn){
            //詰手順、探索情報表示
            if(g_redundant){
                printf("詰みました。駒余りのため詰手順は参考。\n");
            }
            else{
                printf("詰みました。%u手詰め\n",tdata.sh);
            }
            printf("消費時間 %lu(sec)\n", (finish-start)/CLOCKS_PER_SEC);
            printf("探索局面数 %llu\n", g_tsearchinf.nodes);
            printf("詰探索木ノード数　%llu\n", g_tbase->pr_num);
            printf("局面表登録数　%llu\n\n", g_tbase->num);
            tsume_print(&g_sdata, g_tbase, TP_NONE);
            if(g_summary) create_search_report();
            if(st_display) tsume_debug(&g_sdata, g_tbase);
        }
        else if(!tdata.dn) {
            printf("不詰です。\n");
        }
        else {
            printf("詰みを発見できませんでした。（pn=%u, dn=%u, sh=%u)\n",
                   tdata.pn, tdata.dn, tdata.sh);
        }
        
        initialize_tbase(g_tbase);
        init_mtt(g_mtt);
        mlist_free_stack();
        mvlist_free_stack();
        return 0;
    }
}

void print_version (void)
{
    printf("%s %s\n",PROGRAM_NAME,VERSION_INFO);
    return;
}
