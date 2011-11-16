#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>

/* Constantes */
#define MAXLINE    1024   /* Tamanho máximo da linha                         */
#define MAXARGS     128   /* Numero máximo de argumentos da linha de comando */
#define MAXJOBS      16   /* Número máximo de jobs                           */
#define MAXJID    1<<16   /* Número máximo de jobID                          */

/* Estados dos Jobs */
#define UNDEF 0 /* indefinido */
#define FG 1    /* foreground */
#define BG 2    /* background */
#define ST 3    /* stopped    */

char prompt[] = "usuario:~";    /* Nome do shell */
int verbose = 0;
int nextjid = 1;
int flagbg = 0;
char sbuf[MAXLINE];         /* for composing sprintf messages */

struct job_t {              /* Estrutura da Job              */
    pid_t pid;              /* Job PID                       */
    int jid;                /* Job ID                        */
    int state;              /* Estados: UNDEF, BG, FG, or ST */
    char cmdline[MAXLINE];  /* Linha de comando              */
};
struct job_t jobs[MAXJOBS]; /* Lista de Jobs */


/* Funções */
void eval(char*, int, int[], int[]);
int builtin_cmd(char**);
void builtin_bgfg(char**);
void builtin_cd(char**);
void builtin_pwd(char**);
void waitfg(pid_t);
void sigchld_handler(int);
void sigtstp_handler(int);
void sigint_handler(int);
int parseline(const char*, char**);
void sigquit_handler(int);

void limpajob(struct job_t*);
void inicializaAllJobs(struct job_t*);
int maxjid(struct job_t*);
int addjob(struct job_t*, pid_t, int, char*);
int excluijob(struct job_t*, pid_t);
pid_t fgpid(struct job_t*);
struct job_t *getjobpid(struct job_t*, pid_t);
struct job_t *getjobjid(struct job_t*, int);
int pidtojid(pid_t);
void builtin_jobs(struct job_t*);
typedef void handler_t(int);
handler_t *signal(int, handler_t*);
void check_redir (char*);
void redir ();

/* Funcoes e constantes para redirecionamento */
int redir_out=0; /* Se (>), a variável redir_out será setada para 1    */
int redir_in=0;  /* Se (<), a variável redir_in será setada para 1     */
int redir_app=0; /* Se (>>), a variável redir_app será setada para 1   */
int redir_err=0; /* Se (2>), a variável redir_err será setada para 1   */
int redir_erra=0; /*Se (2>>), a variável redir_erra será setada para 1 */
int fpin, fpout, fperr; /* fp -> file pointer */
fpos_t pos_in, pos_out; /* posicao do buffer de stdin e stdout */


/*	Main	*/
int main(int argc, char **argv, char **envp){
	char c;
	char cmdline[MAXLINE];
	int fdstd[2];
	int continua;
	

	/* Redirciona stderr to stdout */
	dup2(1, 2);

	/*Iniciando signal handlers*/
	signal(SIGINT,  sigint_handler);   /* ctrl-c */
	signal(SIGTSTP, sigtstp_handler);  /* ctrl-z */
	signal(SIGCHLD, sigchld_handler);  /* verifica sinal processo filho*/
	signal(SIGQUIT, sigquit_handler);
	inicializaAllJobs(jobs);

	/*Limpa tela antes de tudo*/
    	if(fork() == 0){
        	execve("/usr/bin/clear", argv, envp);
        	exit(1);
    	} 
	else wait(NULL);
	
	flagbg = 0;
	while (1) {
		//inicializando flagbg
		flagbg = 0;
		printf("%s", prompt);
		strcpy(argv[0], "pwd");
		builtin_pwd(argv);
		printf("$ ");
		fflush(stdout);
		fgets(cmdline, MAXLINE, stdin);
		continua = 1;
		while(continua == 1){
			flagbg = 0;
			if(cmdline[0] == '\n'){
				printf("%s", prompt);
				strcpy(argv[0], "pwd");
				builtin_pwd(argv);
				printf("$ ");
				fflush(stdout);
				fgets(cmdline, MAXLINE, stdin);
			}
				
			else continua = 0;
				
		}
		check_redir(cmdline);
		eval(cmdline, 0, NULL, fdstd);
		redir();
		fflush(stdout);

		signal(SIGINT,  sigint_handler);   /* ctrl-c */
		signal(SIGTSTP, sigtstp_handler);  /* ctrl-z */
		signal(SIGCHLD, sigchld_handler);  /* verifica sinal processo filho */
		signal(SIGQUIT, sigquit_handler);
	}

	exit(0);
}

/* Implementacao de funcoes */

/* Evaluate command line - executa a command line */
void eval(char *cmdline, int frompipe, int ifd[2], int fdstd[2]) {
	char **argv;
	int bg, i, pid;
	char for_cmdline[MAXLINE];
	char *after_cmdline;
	int topipe = 0; /*Flag para o pipe*/
	int pfd[2]; /*File descriptor do pipe*/

	if(frompipe ){
		fdstd[0] = dup(fileno(stdin));
		dup2( ifd[0], 0);
	}

	/*Quebrando a command line para o pipe*/
	if((after_cmdline = strstr(cmdline, "|")) != NULL){
		printf("\n%s\n",after_cmdline);
		topipe = 1;
		strncpy(for_cmdline, cmdline, strlen(cmdline) - strlen(after_cmdline) );
		for_cmdline[strlen(cmdline) - strlen(after_cmdline)] = '\0';
		strcpy( after_cmdline, after_cmdline+1 );
		if(pipe(pfd) == -1){
			printf("\nErro pipe!\n");
			exit(EXIT_FAILURE);
		}
		fdstd[1] = dup(fileno(stdout));
		dup2(pfd[1], 1);
	}
	else{
		strcpy(for_cmdline, cmdline);
	}

	/*Alocando vetor argv de tamanho MAXARGS e cada posição de tamanho MAXLINE*/
	argv = (char**) calloc( MAXARGS, MAXLINE );
	for( i = 0; i < MAXARGS; i++ )
		argv[i] = (char*) calloc( MAXLINE, sizeof(char));

	/*Pega a command line e em seguida a trata na função parseline*/
	/**** retornando se esta em background ****/
	bg = parseline(for_cmdline, argv);

	/*Caso não for built in command executa o IF*/
	/* Se for built in, IF não é executado e função builtin_command executa o comando*/
	if (!builtin_cmd(argv)){
		sigset_t newmask; /* Cria nova mascara */

		sigemptyset (&newmask); /* Limpa a variável */
		sigaddset(&newmask, SIGCHLD); /* Adiciona SIGCHLD par a anova variavel */
		sigprocmask(SIG_BLOCK, &newmask, NULL); /* Adiciona a nova mascara aos sinais de que a função está bloqueando */

		/* Se não for um comando interno cria um novo processo para executar um comando externo */
		pid = fork();
		/* Se pid for zero, processo FOI criado */
		if ( pid == 0 ){
			sigprocmask(SIG_UNBLOCK, &newmask, NULL); /* remove SIGCHLD da lista de sinais bloqueados */

			/*O proximo passo eh criar um grupo de processos, assim quando voce matar um processo, ir
			matar todos os filhos dele. Processos do mesmo grupo tem o mesmo ID.*/
			if( setpgid(0,0) ){
				printf("Erro");
			}

			/*O primeiro argumento de execvp eh o nome do programa que deve ser executado, o segundo eh a lista de argumentos
			passada para o programa, e o terceiro eh a lista de variaveis de ambiente, definida no inicio do main.*/
			if (execvp(argv[0], argv)) {
				printf("%s: Comando não encontrado\n",argv[0]);
				exit(1);
			}
		}

		else{
			/* Adiciona processo a job list */
			if (flagbg==1) {
				addjob(jobs,pid,BG,cmdline);
			}
			else{
				addjob(jobs,pid,FG,cmdline);
			}
			sigprocmask(SIG_UNBLOCK, &newmask, NULL); /* remove SIGCHLD da lista de bloqueados */
		}

		if(flagbg==1) {
			printf("[%d] (%d) %s\n", pidtojid(pid), pid, cmdline);
		}
		else
			waitfg(pid);

	}
	if(frompipe){
		dup2(fdstd[0],fileno(stdin));
	}
	if(topipe){
		dup2(fdstd[1],fileno(stdout));
		close(pfd[1]);
		eval(after_cmdline, 1, pfd, fdstd);
	}

}

/* Função que trata a command line, separa o comando e trata seus parâmetros */
int parseline(const char *cmdline, char **argv) {
	
	
	static char array[MAXLINE];
	char *buf = array;
	char *delim;
	int argc;
	int bg;

	strcpy(buf, cmdline);
	buf[strlen(buf)-1] = ' ';

    	while (*buf && (*buf == ' ')){
		buf++;
	}

	argc = 0;
	if (*buf == '\'') {
		buf++;
		delim = strchr(buf, '\'');
	}
	else {
		delim = strchr(buf, ' ');
	}

	while (delim) {
		argv[argc++] = buf;
		*delim = '\0';
		buf = delim + 1;
		while (*buf && (*buf == ' ')){
			buf++;
		}

		if (*buf == '\'') {
			buf++;
			delim = strchr(buf, '\'');
		}
		else {
			delim = strchr(buf, ' ');
		}
	}
	argv[argc] = NULL;
	if (argc == 0){
		return 1;
	}

	if ((bg = (*argv[argc-1] == '&')) != 0) {
		argv[argc-1] = NULL;
		flagbg = 1;
	}

	return bg;
}

/* Função que verifica e trata os comandos internos (built in command), */
/* retornando "1" se é built in command e 0 caso nao*/
int builtin_cmd(char **argv){

	/* cd - navegar entre pastas */
	if( (strcmp(argv[0],"cd") == 0) ){
		builtin_cd(argv);
		return(1);
	/* fg e bg - Enviar processo para fg e bg*/
	}else if( (strcmp(argv[0],"fg") == 0) || (strcmp(argv[0],"bg") == 0) ){
		builtin_bgfg(argv);
		return(1);
	/* jobs - listar processo */
	}else	if( strcmp(argv[0],"jobs") == 0){
		builtin_jobs(jobs);
		return(1);
	/* sair do shell */
	}else if (strcmp(argv[0],"exit") == 0){
		exit(1);
	/*pwd - exibe a localização atual*/
	}else if (!strcmp(argv[0], "pwd")){
		builtin_pwd(argv);
		printf("\n");
		return(1);
	}
	return(0);
}

/* Executa o comando cd */
void builtin_cd(char **argv) {

	/* Utilizando o comando chdir é possivel entrar na pasta desejada */
	/* retorna 0 caso o caminho não exista */
	if(chdir(argv[1]) != 0) {
		printf("Caminho não encontrado!\n");
	}
}

/* Executa comando pwd */
void builtin_pwd(char **argv){
	char saida[MAXLINE];

	/* Grava na string saida a localizacao atual */
	getcwd(saida, MAXLINE);
	printf("%s", saida);

}

/* Executa os comando bg e fg */
void builtin_bgfg(char **argv){
	int pid, jid;
	int i = 0;

	if( argv[1][0] == '%' ){
		jid = atoi(argv[1]+1);
		while( (i < MAXJOBS) && (jobs[i].jid != jid) )
		i++;
	}
	else{
		pid = atoi(argv[1]);
		while( (i < MAXJOBS) && (jobs[i].pid != pid) )
		i++;
	}

	if( (i >= 0) && (i < MAXJOBS) ){
		int actualstate = jobs[i].state;

		if ( (actualstate == ST) && (strcmp(argv[0],"bg") == 0) ){
			kill( -jobs[i].pid, SIGCONT);
			jobs[i].state = BG;
			printf("[%d] (%d) %s\n", jobs[i].jid, jobs[i].pid, jobs[i].cmdline);
		}
		else if( strcmp(argv[0],"fg") == 0 ){
			kill( -jobs[i].pid, SIGCONT);
			jobs[i].state = FG;
			waitfg(jobs[i].pid);
		}
	}
	else{
		if( argv[1][0] == '%' ){
			printf("%s: Job não encontrado\n", argv[1]);
		}
		else{
			printf("(%s): Processo não encontrado\n", argv[1]);
		}
	}
	return;
}

/*bloqueia pid do processo até que não é mais o processo em primeiro plano */
void waitfg(pid_t pid){
	struct job_t *existe;

	while (1) {
		existe = getjobpid(jobs,pid);
		/*Se existe for NULL entao o processo foi finalizado e nao esta mais na lista de processos*/
		if(existe == NULL){
			break; /*Volta para a execucao normal do programa*/
		}

		/*Se o estado de job for diferente de FG, entao ou ele foi para BG ou foi parado por um SIGSTOP*/
		if ( (*existe).state != FG) {
			break; /*Volta para a execucao normal do programa*/
		}
		sleep(1);
	}
}

/*****************************/
/*Trata processos zumbis job list*/
void sigchld_handler(int sig){
	int estado;
	int pid;

	while (1) {
		pid = waitpid (-1, &estado, WNOHANG | WUNTRACED | WCONTINUED); /*Pega o pid do job que gerou o sinal*/

		if (pid<=0)
			break;
		else { /*Se a funcao retornar um pid valido (WNOHANG retorna um pid invalido caso nao haja sinais para serem tratados nesse momento)*/

			struct job_t *atual;
			atual = getjobpid(jobs, pid);
			if (atual!=NULL) {
				if (WIFSTOPPED(estado)){
					atual->state = ST;
				}
				else if (WIFCONTINUED(estado)){
					atual->state = BG;
				}
				/******************************************************************************/
				/*WIFEXITED - retorna true se o processo filho terminar normalmente           */
				/*WIFISIGNALED retorna true se o processo filho receber um sinal para terminar*/
				else if(WIFEXITED(estado) || WIFSIGNALED(estado)){
					excluijob(jobs, atual->pid);
				}
				fflush(stdout);
			}
		}// fim do else
	}//fim do while
	return;

}

/*O kernel envia para o shell SIGINT toda vez que o ctrl+c é pressionado no teclado*/
/*com isso enviamos o sinal SIGINT para o processo interrompendo ele*/
void sigint_handler(int sig){
	int i;

	for (i = 0; i < MAXJOBS; i++) {
		if (jobs[i].state == FG) {
				if( !kill(-jobs[i].pid, sig) ){ /* Envia o sinal SIGINT para o PID indicado */
					printf(" Job [%d] (%d) encerrado!\n", jobs[i].jid, jobs[i].pid);
					limpajob(&jobs[i]);
					nextjid = maxjid(jobs)+1;
				}
				else{
					fprintf(stderr," Job [%d] (%d) não pode ser encerrado\n", jobs[i].jid, jobs[i].pid);
				}
				return;
		}
  }

  return;
}
/*O kernel envia para o shell SIGTSTP toda vez que o ctrl+z é pressionado no teclado*/
/*com isso enviamos o sinal SIGTSTP para o processo e setamos seu estado para ST*/
void sigtstp_handler(int sig){
	int i;

	for (i = 0; i < MAXJOBS; i++) {
		if (jobs[i].state == FG) {
				if( !kill(-jobs[i].pid, sig) ){ /* Envia o sinal SIGTSTP para o PID indicado */
					printf(" Job [%d] (%d) parado!\n", jobs[i].jid, jobs[i].pid);
					jobs[i].state = ST;
				}
				else{
					fprintf(stderr," Job [%d] (%d) não pode ser parado!\n", jobs[i].jid, jobs[i].pid);
				}
				return;
		}
  }

  return;
}


/* Clear the entries in a job struct */
void limpajob(struct job_t *job) {
	job->pid = 0;
	job->jid = 0;
	job->state = UNDEF;
	job->cmdline[0] = '\0';
}

/* Inicializar todos os jobs um a um */
void inicializaAllJobs(struct job_t *jobs) {
	int i;

	for (i = 0; i < MAXJOBS; i++)
			limpajob(&jobs[i]);
}

/* Retorna o maior ID alocado.*/
int maxjid(struct job_t *jobs) {
	int i, max=0;

	for (i = 0; i < MAXJOBS; i++){
		if (jobs[i].jid > max){
			max = jobs[i].jid;
		}
	}

	return max;
}

/* Adiciona um job à lista. Se adicionou retorna "1", senão "0" */
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline) {
	int i;

	/* Verifica se o pid é valido, caso 0 invalido */
	if (pid < 1){
		return 0;
	}

	/* Verifica todas as posições de jobs, até encontrar uma vazia*/
	for (i = 0; i < MAXJOBS; i++) {
		if (jobs[i].pid == 0) {
			/* Guardando os valores */
			jobs[i].pid = pid;
			jobs[i].state = state;
			jobs[i].jid = nextjid++;
			strcpy(jobs[i].cmdline, cmdline);

			if (nextjid > MAXJOBS){
				nextjid = 1;
			}

			return 1;
		}
	}
	/* Caso não encontrou uma posição vazia imprime o texto seguinte */
	/* Imprimir que o número de jobs chegou ao limite */
	printf("O número máximo de jobs já foi criado\n");

	return 0;
}

/* Deleta um determinado job através do pid */
int excluijob(struct job_t *jobs, pid_t pid){
	int i;

	if (pid < 1){
		return 0;
	}

	for (i = 0; i < MAXJOBS; i++) {
		if (jobs[i].pid == pid) {
			limpajob(&jobs[i]);
			nextjid = maxjid(jobs)+1;
			return 1;
		}
	}

	return 0;
}

/* Retorna o PID do atual job em foreground, ou 0 caso não exista */
pid_t fgpid(struct job_t *jobs) {
	int i;

	for (i = 0; i < MAXJOBS; i++){
		if (jobs[i].state == FG){
			return jobs[i].pid;
		}
	}

	return 0;
}

/* Busca um job por PID na job list */
struct job_t *getjobpid(struct job_t *jobs, pid_t pid){
	int i;

	if (pid < 1){
		return NULL;
	}
	for (i = 0; i < MAXJOBS; i++){
		if (jobs[i].pid == pid){
			return &jobs[i];
		}
	}

	return NULL;
}

/* Busca um job por JID na job list */
struct job_t *getjobjid(struct job_t *jobs, int jid){
	int i;

	if (jid < 1){
		return NULL;
	}
	for (i = 0; i < MAXJOBS; i++){
		if (jobs[i].jid == jid){
			return &jobs[i];
		}
	}

	return NULL;
}

/* ID para job ID */
int pidtojid(pid_t pid){
	int i;

	if (pid < 1){
		return 0;
	}
	for (i = 0; i < MAXJOBS; i++){
		if (jobs[i].pid == pid) {
			return jobs[i].jid;
		}
	}

	return 0;
}

/* Listando os Jobs */
void builtin_jobs(struct job_t *jobs) {
	int i;

	for (i = 0; i < MAXJOBS; i++) {
		/* Se job existe */
		if (jobs[i].pid != 0) {
			/* Imprime JID e PID*/
			printf("[%d] (%d) ", jobs[i].jid, jobs[i].pid);
			/* Imprime os estados do Job */
			switch (jobs[i].state) {
				/*BG*/
				case BG:
					printf("Running ");
					break;
				/*FG*/
				case FG:
					printf("Foreground ");
					break;
				/*Stopped*/
				case ST:
					printf("Stopped ");
					break;
				/*Em caso de erro*/
				default:
					printf("listjobs: Internal error: job[%d].state=%d ", i, jobs[i].state);
			}
			/*Imprime o comando na sequencia*/
			printf("  %s\n", jobs[i].cmdline);
		}
	}
}

handler_t *signal(int signum, handler_t *handler){
	struct sigaction action, old_action;

	action.sa_handler = handler;
	sigemptyset(&action.sa_mask);
	action.sa_flags = SA_RESTART;

	if (sigaction(signum, &action, &old_action) < 0){
		printf("Erro Sinal!\n");
	}

	return (old_action.sa_handler);
}

void sigquit_handler(int sig){
	printf("Finalizando, após a recepção do sinal SIGQUIT\n");
	exit(1);
}

/***********************************************
 * Funcoes para Redirecionamento
 **********************************************/

/*Esta funcao checa se o comando digitado pede redirecionamento. Se sim, ele vai setar as flags corretas e realizar
* o redirecionamento adequado.*/
void check_redir (char *cmdline){
	char **argv;
	char file_in[50], file_out[50], file_app[50], file_err[50]; //declarados separadamente porque podemos ter dois casos ao mesmo tempo
	char aux[1024] = {0};
	int i;

	/* Dynamically allocate the argv to its maximum possible sizes */
	argv = (char**) calloc( MAXARGS, MAXLINE );
	for( i = 0; i < MAXARGS; i++ )
		argv[i] = (char*) calloc( MAXLINE, sizeof(char));
	/* End of allocation */

	parseline(cmdline, argv);

	for(i=0; i < MAXARGS; i++){
		if (argv[i]!=NULL) {
			if (strcmp(argv[i],"<") == 0){
				redir_in=1;
				strncpy(file_in, argv[i+1], strlen(argv[i+1]) + 1);
				sprintf(argv[i], " ");
				sprintf(argv[i+1], " ");
			}
			if ( (strcmp(argv[i],">>") == 0) || (strcmp(argv[i],"1>>") == 0) ){
				redir_app=1;
				strncpy(file_app, argv[i+1], strlen(argv[i+1]) + 1);
				sprintf(argv[i], " ");
				sprintf(argv[i+1], " ");
			}
			if ( (strcmp(argv[i],">") == 0) || (strcmp(argv[i],"1>") == 0) ){
				redir_out=1;
				strncpy(file_out, argv[i+1], strlen(argv[i+1]) + 1);
				sprintf(argv[i], " ");
				sprintf(argv[i+1], " ");
			}

			if (strcmp(argv[i],"2>") == 0){
				redir_err=1;
				strncpy(file_err, argv[i+1], strlen(argv[i+1]) + 1);
				sprintf(argv[i], " ");
				sprintf(argv[i+1], " ");
			}
			if (strcmp(argv[i],"2>>") == 0){
				redir_erra=1;
				strncpy(file_err, argv[i+1], strlen(argv[i+1]) + 1);
				sprintf(argv[i], " ");
				sprintf(argv[i+1], " ");
			}
		}
	}

	//Fecha o stdin e abre do arquivo dado
	if (redir_in==1){
		fflush(stdin);
		fgetpos(stdin, &pos_in);
		fpin = dup(fileno(stdin)); //dup copia stdin para o fdin
		freopen(file_in, "r", stdin);
	}

	//Fecha o stdout e abre do arquivo dado
	if (redir_out==1){
		fflush(stdout);
		fgetpos(stdout, &pos_out);
		fpout = dup(fileno(stdout));
		freopen(file_out, "w", stdout);
	}
	//Fecha o stdout e abre do arquivo dado
	else if (redir_app==1){
		fflush(stdout);
		fgetpos(stdout, &pos_out);
		fpout = dup(fileno(stdout));
		freopen(file_app, "a", stdout); //append
	}

	//Fecha o stderr e abre do arquivo dado
	if (redir_err==1){
		fflush(stderr);
		fgetpos(stdout, &pos_out);
		fperr = dup(fileno(stderr)); //dup copia stderr para o fderr
		freopen(file_err, "w", stderr);
	}

	//Fecha o stderr e abre o arquivo dado, caso seja solicitado um redirecionamento de erro com append
	else if (redir_erra==1){
		fflush(stderr);
		fgetpos(stdout, &pos_out);
		fperr = dup(fileno(stderr)); //dup copia stderr para o fderr
		freopen(file_err, "a", stderr);
	}

	//Os argv são anexados a uma string aux que no passo seguinte será atribuída à cmdline.
	//Isso é feito porque quebramos a linha de comando para a leitura inicial e agora precisamos monta-la novamente
	//para que possa ser utilizada na funcao eval.
	for (i=0; i< MAXARGS; i++){
		if (argv[i]!=NULL) {
			strcat(aux,argv[i]);
			strcat(aux, " ");
		}
	}
	strcpy(cmdline, aux);// cmdline recebe aux
}

/* Esta funcao, chamada depois de eval, irá resetar as flags para redirecionamento.*/
void redir (){
	// Reseta STDOUT
	if (redir_out || redir_app) {
		fflush(stdout); //limpou o que tinha no arquivo
		dup2(fpout, fileno(stdout)); //voltou o stdout para o original dele
		close (fpout);//fechou o ponteiro temporario
		clearerr (stdout);//limpou o erro (caso precise)
		fsetpos (stdout, &pos_out);//voltou para a posicao que estava antes
	}
	// Reseta STDIN
	if (redir_in) {
		fflush(stdin);
		dup2(fpin, fileno(stdin));
		close(fpin);
		clearerr(stdin);
		fsetpos(stdin, &pos_in);
	}
	// Reseta STDERR
	if (redir_err || redir_erra){
		fflush(stderr);
		dup2(fperr, fileno(stderr));
		close(fperr);
		clearerr(stderr);
		fsetpos(stderr, &pos_in);
	}

	//Reseta as flags
	redir_out=0;
	redir_in=0;
	redir_app=0;
	redir_err=0;
	redir_erra=0;
}
