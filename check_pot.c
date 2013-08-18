#include <wiringPiSPI.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <math.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <getopt.h>
#include <signal.h>
#include <syslog.h>

#define SPI_CHANNEL 0
#define SAMPLE 256

int readVoltage(int diff, int channel) {
    unsigned char buf[3];

    buf[0] = 0;
    buf[1] = 0;
    buf[2] = 0;

    buf[0] |= 1 << 2;
    buf[0] |= diff ? 0 : 1<< 1;
    buf[0] |= (channel >> 2) & 0x01;
    buf[1] |= ((channel >> 1) & 0x01) << 7;
    buf[1] |= ((channel >> 0) & 0x01) << 6;

    int ret=wiringPiSPIDataRW (SPI_CHANNEL,buf,3);

    int result = ((buf[1] & 0b1111) << 8) | buf[2];	
    return result;
}

double power_spectral(int f[], int len, int k) {
    int i;
    double re=0,im=0;
    for(i = 0; i < len; i++) {
        re += f[i] * cos(2*M_PI*i*k/len);
        im += - f[i] * sin(2*M_PI*i*k/len);
    }
    //printf("%d, %f, %f, %f\n",k, re, im, sqrt(re*re+im*im));
    return sqrt(re*re+im*im);
}

int check_power() {
    int i;
    int result[SAMPLE];
    for(i = 0;  i < SAMPLE; i++) {
        result[i] = readVoltage(0, 0);
        usleep(1000);
    }
    double ps = power_spectral(result, SAMPLE, 15);
    int is_on = ps > 5000;
    syslog(LOG_INFO, "%s %f\n", is_on?"on":"off", ps);
    return is_on;
}

#define FLAG_DAEMONIZE     1
#define FLAG_KILL          2

int main (int argc, char **argv) {
    /**
     * opt:
     *   コマンドラインオプションのコード
     *   command -h としたとき、'h' のコードが入る
     *   command --help としたときには、struct longopt 構造体の、
     *   第４メンバーが代入される
     *
     * flag:
     *   フラグ
     * 
     * nochdir:
     *   フラグが 0 でなければカレントディレクトリを "/" (ルート) にしない
     *
     * noclose:
     *   フラグが 0 でなければ 標準入力, 標準出力, 標準エラー を閉じない
     *
     * pid_file_path:
     *   プロセス ID を記録するファイルへの絶対パス
     *
     * pid_file:
     *  プロセス ID を記録するための、ファイルディスクリプタ
     * 
     * pid:
     *   プロセス ID を格納する変数
     */
    int opt;
    int flag = 0;
    int nochdir = 0;
    int noclose = 0;
    char *pid_file_path = NULL;
    FILE *pid_file;
    pid_t pid;
    char *pot_off_command = NULL;
    char *pot_on_command = NULL;

    /**
     * daemon[PID]: ログメッセージ
     * というように /var/log/messages に出力するようログを開く
     */
    openlog ("check_pot", LOG_PID, LOG_DAEMON);

    /**
     * コマンドライン引数のテーブル
     * 第一メンバ:
     *   オプション名 (--help の場合 "help"）
     *
     * 第２メンバ:
     *   no_argument | required_argument | optional_argument
     *   それぞれ、値を必要としない、必要とする、オプションとなる
     *
     * 第３メンバ:
     *   フラグセット用のポインタ
     *   基本的に使わないので、NULL にしておけばいい。
     *
     * 第４引数:
     *   opt に代入されるコード (int 型)
     *   ショートオプション（-h のような形式）とおなじ 'h' としておくと楽
     */
    const struct option longopt[] = {
        {"help", no_argument, NULL, '?'},
        {"daemonize", no_argument, NULL, 'd'},
        {"kill", no_argument, NULL, 'k'},
        {"pidfile", required_argument, NULL, 'p'},
        {NULL, 0, NULL, 0}
    };

    /**
     * デーモン起動時のオプションをパース
     *
     */
    while ((opt = getopt_long (argc, argv, "p:dk", longopt, NULL)) != -1)
    {
        switch (opt)
        {
            case 'p':
                pid_file_path = optarg;
                break;

            case 'd':
                flag = FLAG_DAEMONIZE;
                break;

            case 'k':
                flag = FLAG_KILL;
                break;

            default:
                break;
        }
    }

    /**
     * オプションが指定されなかった場合にエラーを表示し終了
     */
    if (pid_file_path == NULL)
    {
        printf ("--pid-file required\n");
        return -1;
    }

    if (flag == FLAG_KILL)
    {
        pid_file = fopen (pid_file_path, "r");
        if (pid_file != NULL)
        {
            fscanf (pid_file, "%d\n", &pid);
            fclose (pid_file);
            if (kill (pid, SIGKILL) == 0)
            {
                syslog (LOG_INFO, "daemon stopped.\n");
            }
        }
        else
        {
            syslog (LOG_ERR, "no daemon started.\n");
            return -1;
        }
        return 0;
    }

    pot_off_command = getenv("POT_OFF_COMMAND");
    pot_on_command = getenv("POT_ON_COMMAND");
    

    /**
     * デーモンプロセスの立ち上げ
     */
    if (flag == FLAG_DAEMONIZE)
    {
        if (daemon (nochdir, noclose) == -1)
        {
            syslog (LOG_ERR, "failed to launch daemon.\n");
            return -1;
        }
    }

    syslog (LOG_INFO, "daemon started.\n");

    /**
     * getppid() を実行すると、1 が帰ってくるはず
     * daemon() の実行によって、fork() されたプロセスが、
     * いまここにいて、親プロセスは先に _exit() を呼び出して
     * 死んでしまっているので、init プロセスの養子になっている。
     * init プロセスは、Linux 起動時に最初に起動されるため PID は必ず 1
     */

    /**
     * このプロセス（デーモン）の PID を取得し、
     * ファイルに PID を記録
     */
    pid = getpid ();
    pid_file = fopen (pid_file_path, "w+");
    if (pid_file != NULL)
    {
        fprintf (pid_file, "%d\n", pid);
        fclose (pid_file);
    }
    else
    {
        syslog (LOG_ERR, "failed to record process id to file.\n");
        return -1;
    }

    /**
     * デーモンの実際の処理部分
     * ここをからループにしたり、待ち時間をなくすと CPU 使用率が高くなるので注意
     * 実際のデーモンは、ここで停止しタイマーによって起こしてもらったり
     * イベントドリブンな設計で、なにか変化があるまで動作を停止していることが多い
     */
    int fd = wiringPiSPISetup(SPI_CHANNEL, 1000000);
    if(fd < 0) {
        syslog(LOG_ERR, "SPI Setup failed: %s\n", strerror (errno));
        exit(-1);
    }

    int prev_power, power; 
    prev_power = power = check_power();
    while(1) {
        sleep(power ? 5 : 30);
        power = check_power();
        if(prev_power != power) {
            syslog(LOG_INFO, "Power state changed! %d -> %d", prev_power, power);
            if(power) {
                if(pot_on_command) {
                    system(pot_on_command);
                }
            } else {
                if(pot_off_command) {
                    system(pot_off_command);
                }
            }
        }
        prev_power = power;
    }

    return 0;
}
