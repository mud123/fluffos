#define NO_MALLOC
#define NO_SOCKETS
#define NO_OPCODES
#define EDIT_SOURCE
#include "std.h"
#include "lex.h"
#include "preprocess.h"
#include "make_func.h"
#include "cc.h"

char *outp;
static int buffered = 0;
static int nexpands = 0;

FILE *yyin = 0, *yyout = 0;

#define SYNTAX "edit_source [-process file] [-options] [-malloc] [-build_func_spec 'command'] [-build_efuns] [-configure]\n"

/* The files we fool with.  (Actually, there are more.  See -process).
 *
 * TODO: teach this file how to fix bugs in the source code :)
 */
#define OPTIONS_H         "options.h"
#define OPTION_DEFINES    "option_defs.c"
#define FUNC_SPEC         "func_spec.c"
#define FUNC_SPEC_CPP     "func_spec.cpp"
#define EFUN_TABLE        "efunctions.h"
#define OPC_PROF          "opc.h"
#define OPCODES           "opcodes.h"
#define EFUN_PROTO        "efun_protos.h"
#define EFUN_DEFS         "efun_defs.c"

#define PRAGMA_NOTE_CASE_START 1

int num_packages = 0;
char *packages[100];
char ppchar;

char *current_file = 0;
int current_line;

int grammar_mode = 0; /* which section we're in for .y files */
int in_c_case, cquote, pragmas, block_nest;

char yytext[MAXLINE];
static char defbuf[DEFMAX];

typedef struct incstate_t {
    struct incstate_t *next;
    FILE *yyin;
    int line;
    char *file;
} incstate;

static incstate *inctop = 0;

#define CHAR_QUOTE 1
#define STRING_QUOTE 2

void mf_fatal P1(char *, str)
{
    fprintf(stderr, "%s", str);
    exit(1);
}

void yyerror P1(char *, str)
{
    fprintf(stderr, "%s:%d: %s\n", current_file, current_line, str);
    exit(1);
}

void yywarn P1(char *, str)
{
    fprintf(stderr, "%s:%d: %s\n", current_file, current_line, str);
    exit(1);
}

void yyerrorp P1(char *, str)
{
    char buff[200];
    sprintf(buff, str, ppchar);
    fprintf(stderr, "%s:%d: %s\n", current_file, current_line, buff);
    exit(1);
}

static void add_input P1(char *, p)
{
    int l = strlen(p);

    if (outp - l < defbuf) yyerror("Macro expansion buffer overflow.\n");
    strncpy(outp - l, p, l);
    outp -= l;
}

#define SKIPW(foo) while (isspace(*foo)) foo++;

static char *skip_comment(tmp, flag)
     char *tmp;
     int flag;
{
    int c;
    
    for (;;) {
	while ((c = *++tmp) !=  '*') {
	    if (c == EOF) yyerror("End of file in a comment");
            if (c == '\n') {
                nexpands = 0;
                current_line++;
		if (!fgets(yytext, MAXLINE - 1, yyin)) yyerror("End of file in a comment");
		if (flag && yyout) fputs(yytext, yyout);
		tmp = yytext - 1;
	    }
	}
	do {
	    if ((c = *++tmp) == '/')
                return tmp + 1;
            if (c == '\n') {
                nexpands = 0;
                current_line++;
		if (!fgets(yytext, MAXLINE - 1, yyin)) yyerror("End of file in a comment");
		if (flag && yyout) fputs(yytext, yyout);
		tmp = yytext - 1;
	    }
	} while (c == '*');
    }
}

static void refill()
{
    register char *p, *yyp;
    int c;

    if (fgets(p = yyp = defbuf + (DEFMAX >> 1), MAXLINE - 1, yyin)){
      while (((c = *yyp++) != '\n') && (c != EOF)){
          if (c == '/'){
              if ((c = *yyp) == '*') {
                  yyp = skip_comment(yyp, 0);
                  continue;
              }
              else if (c == '/') break;
          }
          *p++ = c;
      }
    }
    else yyerror("End of macro definition in \\");
    nexpands = 0;
    current_line++;
    *p = 0;
    return;
}

static void handle_define()
{
    char namebuf[NSIZE];
    char args[NARGS][NSIZE];
    char mtext[MLEN];
    char *end;
    register char *tmp = outp, *q;

    q = namebuf;
    end = q + NSIZE - 1;
    while (isalunum(*tmp)){
	if (q < end) *q++ = *tmp++;
	else yyerror("Name too long.\n");
    }
    if (q == namebuf) yyerror("Macro name missing.\n");
    *q = 0;
    if (*tmp == '(') {            /* if "function macro" */
        int arg;
        int inid;
        char *ids = (char *) NULL;

        tmp++;                    /* skip '(' */
        SKIPW(tmp);
        if (*tmp == ')') {
            arg = 0;
	} else {
            for (arg = 0; arg < NARGS;) {
                end = (q = args[arg]) + NSIZE - 1;
		while (isalunum(*tmp) || (*tmp == '#')){
		    if (q < end) *q++ = *tmp++;
		    else yyerror("Name too long.\n");
		}
		if (q == args[arg]){
		    char buff[200];
		    sprintf(buff, "Missing argument %d in #define parameter list", arg + 1);
		    yyerror(buff);
		}
                arg++;
                SKIPW(tmp);
                if (*tmp == ')')
                    break;
                if (*tmp++ != ',') {
                    yyerror("Missing ',' in #define parameter list");
		}
                SKIPW(tmp);
	    }
            if (arg == NARGS) yyerror("Too many macro arguments");
	}
        tmp++;                    /* skip ')' */
	end = mtext + MLEN - 2;
        for (inid = 0, q = mtext; *tmp;) {
            if (isalunum(*tmp)) {
                if (!inid) {
                    inid++;
                    ids = tmp;
		}
	    } else {
                if (inid) {
                    int idlen = tmp - ids;
                    int n, l;

                    for (n = 0; n < arg; n++) {
                        l = strlen(args[n]);
                        if (l == idlen && strncmp(args[n], ids, l) == 0) {
                            q -= idlen;
                            *q++ = MARKS;
                            *q++ = n + MARKS + 1;
                            break;
			}
		    }
                    inid = 0;
		}
	    }
            if ((*q = *tmp++) == MARKS) *++q = MARKS;
            if (q < end) q++;
            else yyerror("Macro text too long");
            if (!*tmp && tmp[-2] == '\\') {
                q -= 2;
                refill();
		tmp = defbuf + (DEFMAX >> 1);
	    }
	}
        *--q = 0;
        add_define(namebuf, arg, mtext);
    } else if (isspace(*tmp) || (!*tmp && (*(tmp+1) = '\0', *tmp = ' '))) {
	end = mtext + MLEN - 2;
        for (q = mtext; *tmp;) {
            *q = *tmp++;
            if (q < end) q++;
            else yyerror("Macro text too long");
            if (!*tmp && tmp[-2] == '\\') {
                q -= 2;
                refill();
		tmp = defbuf + (DEFMAX >> 1);
	    }
	}
        *q = 0;
        add_define(namebuf, -1, mtext);
    } else {
        yyerror("Illegal macro symbol");
    }
    return;
}

#define SKPW while (isspace(*outp)) outp++

static int cmygetc(){
    int c;

    for (;;){
      if ((c = *outp++) == '/'){
          if ((c = *outp) == '*') outp = skip_comment(outp, 0);
          else if (c == '/') return -1;
          else return c;
      } else return c;
    }
}

/* Check if yytext is a macro and expand if it is. */
static int expand_define()
{
    defn_t *p;
    char expbuf[DEFMAX];
    char *args[NARGS];
    char buf[DEFMAX];
    char *q, *e, *b;

    if (nexpands++ > EXPANDMAX) yyerror("Too many macro expansions");
    if (!(p = lookup_define(yytext))) return 0;
    if (p->nargs == -1) {
        add_input(p->exps);
    } else {
        int c, parcnt = 0, dquote = 0, squote = 0;
        int n;

        SKPW;
        if (*outp++ != '(') yyerror("Missing '(' in macro call");
        SKPW;
        if ((c = *outp++) == ')')
            n = 0;
        else {
            q = expbuf;
            args[0] = q;
            for (n = 0; n < NARGS;) {
                switch (c) {
                case '"':
                    if (!squote)
                        dquote ^= 1;
                    break;
                case '\'':
                    if (!dquote)
                        squote ^= 1;
                    break;
                case '(':
                    if (!squote && !dquote)
                        parcnt++;
                    break;
                case ')':
                    if (!squote && !dquote)
                        parcnt--;
                    break;
                case '#':
                    if (!squote && !dquote) {
                        *q++ = c;
                        if (*outp++ != '#') yyerror("'#' expected");
                  }
                    break;
                case '\\':
                    if (squote || dquote) {
                        *q++ = c;
                        c = *outp++;
                  } break;
                case '\n':
                    if (squote || dquote) yyerror("Newline in string");
                  break;
              }
                if (c == ',' && !parcnt && !dquote && !squote) {
                    *q++ = 0;
                    args[++n] = q;
              } else if (parcnt < 0) {
                    *q++ = 0;
                    n++;
                    break;
              } else {
                    if (c == EOF) yyerror("Unexpected end of file");
                    if (q >= expbuf + DEFMAX - 5) {
                        yyerror("Macro argument overflow");
                  } else {
                        *q++ = c;
                  }
              }
                if (!squote && !dquote){
                    if ((c = cmygetc()) < 0) yyerror("End of macro in // comment");
              }
                else c = *outp++;
          }
            if (n == NARGS) {
                yyerror("Maximum macro argument count exceeded");
                return 0;
          }
      }
        if (n != p->nargs) {
            yyerror("Wrong number of macro arguments");
            return 0;
      }
        /* Do expansion */
        b = buf;
        e = p->exps;
        while (*e) {
            if (*e == '#' && *(e + 1) == '#')
                e += 2;
            if (*e == MARKS) {
                if (*++e == MARKS)
                    *b++ = *e++;
                else {
                    for (q = args[*e++ - MARKS - 1]; *q;) {
                        *b++ = *q++;
                        if (b >= buf + DEFMAX) yyerror("Macro expansion overflow");
                  }
              }
          } else {
                *b++ = *e++;
                if (b >= buf + DEFMAX) yyerror("Macro expansion overflow");
          }
      }
        *b++ = 0;
        add_input(buf);
    }
    return 1;
}

static int exgetc()
{
    register char c, *yyp;

    SKPW;
    while (isalpha(c = *outp) || c == '_'){
      yyp = yytext;
      do {
          *yyp++ = c;
      } while (isalnum(c = *++outp) || (c == '_'));
      *yyp = '\0';
      if (!strcmp(yytext, "defined")) {
          /* handle the defined "function" in #/%if */
          SKPW;
          if (*outp++ != '(') yyerror("Missing ( after 'defined'");
          SKPW;
          yyp = yytext;
          if (isalpha(c = *outp) || c == '_'){
              do {
                  *yyp++ = c;
              } while (isalnum(c = *++outp) || (c == '_'));
              *yyp = '\0';
          }
          else yyerror("Incorrect definition macro after defined(\n");
          SKPW;
          if (*outp != ')') yyerror("Missing ) in defined");
          if (lookup_define(yytext))
              add_input("1 ");
          else
              add_input("0 ");
      } else {
          if (!expand_define())
              add_input("0 ");
          else SKPW;
      }
    }
    return c;
}

static int skip_to(token, atoken)
    char *token, *atoken;
{
    char b[20], *p, *end;
    int c;
    int nest;

    for (nest = 0;;) {
        if (!fgets(outp = defbuf + (DEFMAX >> 1), MAXLINE-1,yyin)) {
            yyerror("Unexpected end of file while skipping");
	}
        current_line++;
        if ((c = *outp++) == ppchar) {
	    while (isspace(*outp)) outp++;
	    end = b + sizeof b - 1;
            for (p = b; (c = *outp++) != '\n' && !isspace(c) && c != EOF;) {
		if (p < end) *p++ = c;
	    }
            *p = 0;
            if (!strcmp(b, "if") || !strcmp(b, "ifdef") || !strcmp(b, "ifndef")) {
                nest++;
	    } else if (nest > 0) {
                if (!strcmp(b, "endif"))
                    nest--;
	    } else {
                if (!strcmp(b, token)) {
		    *--outp = c;
                    add_input(b);
		    *--outp = ppchar;
		    buffered = 1;
                    return 1;
		} else if (atoken && !strcmp(b, atoken)) {
		    *--outp = c;
                    add_input(b);
		    *--outp = ppchar;
		    buffered = 1;
		    return 0;
		} else if (!strcmp(b, "elif")) {
		    *--outp = c;
                    add_input(b);
		    *--outp = ppchar;
		    buffered = 1;
                    return !atoken;
		}
	    }
	}
    }
}

#include "preprocess.c"

static void open_input_file P1(char *, fn) {
    if ((yyin = fopen(fn, "r")) == NULL) {
	perror(fn);
	exit(-1);
    }
    if (current_file) free((char *)current_file);
    current_file = (char *) malloc(strlen(fn) + 1);
    current_line = 0;
    strcpy(current_file, fn);
}

static void open_output_file P1(char *, fn) {
    if ((yyout = fopen(fn, "w")) == NULL) {
	perror(fn);
	exit(-1);
    }
}

static void close_output_file() {
    fclose(yyout);
    yyout = 0;
}

static char *protect P1(char *, p) {
    static char buf[1024];
    char *bufp = buf;

    while (*p) {
	if (*p=='\"' || *p == '\\') *bufp++ = '\\';
	*bufp++ = *p++;
    }
    *bufp = 0;
    return buf;
}

static void
create_option_defines() {
    defn_t *p;
    int count = 0;
    int i;

    open_output_file(OPTION_DEFINES);
    fprintf(yyout, "{\n");
    for (i = 0; i < DEFHASH; i++) {
	for (p = defns[i]; p; p = p->next) 
	    if (!(p->flags & DEF_IS_UNDEFINED)) {
		count++;
		fprintf(yyout, "  \"__%s__\", \"%s\",\n", 
			p->name, protect(p->exps));
		if (strncmp(p->name, "PACKAGE_", 8)==0) {
		    int len;
		    char *tmp, *t;
		    
		    len = strlen(p->name + 8);
		    t = tmp = (char *)malloc(len + 1);
		    strcpy(tmp, p->name + 8);
		    while (*t) {
			if (isupper(*t))
			    *t = tolower(*t);
			t++;
		    }
		    if (num_packages == 100) {
			fprintf(stderr, "Too many packages.\n");
			exit(-1);
		    }
		    packages[num_packages++] = tmp;
		}
	    }
    }
    fprintf(yyout,"};\n\n#define NUM_OPTION_DEFS %d\n\n", count);
    close_output_file();
}

static void deltrail(){
    register char *p;

    p = outp;
    while (*p && !isspace(*p) && *p != '\n'){
      p++;
    }
    *p = 0;
}

static void
handle_include P1(char *, name)
{
    char *p;
    static char buf[1024];
    FILE *f;
    incstate *is;

    if (*name != '"') {
        defn_t *d;

        if ((d = lookup_define(name)) && d->nargs == -1) {
            char *q;

            q = d->exps;
            while (isspace(*q))
                q++;
            handle_include(q);
      } else {
            yyerrorp("Missing leading \" in %cinclude");
      }
        return;
    }
    for (p = ++name; *p && *p != '"'; p++);
    if (!*p) yyerrorp("Missing trailing \" in %cinclude");

    *p = 0;
    if ((f = fopen(name, "r")) != NULL) {
        is = (incstate *)
            malloc(sizeof(incstate) /*, 61, "handle_include: 1" */);
        is->yyin = yyin;
        is->line = current_line;
        is->file = current_file;
        is->next = inctop;
        inctop = is;
        current_line = 0;
        current_file = (char *) malloc(strlen(name) + 1 /*, 62, "handle_include: 2" */);
        strcpy(current_file, name);
        yyin = f;
    } else {
        sprintf(buf, "Cannot %cinclude %s", ppchar, name);
        yyerror(buf);
    }
}

static void
handle_pragma P1(char *, name)
{
    int i;

    if (!strcmp(name, "auto_note_compiler_case_start"))
        pragmas |= PRAGMA_NOTE_CASE_START;
    else if (!strcmp(name, "no_auto_note_compiler_case_start"))
        pragmas &= ~PRAGMA_NOTE_CASE_START;
    else if (!strcmp(name, "insert_packages")) {
	fprintf(yyout, "SRC=");
	for (i=0; i < num_packages; i++)
	    fprintf(yyout, "%s.c ", packages[i]);
	fprintf(yyout, "\nOBJ=");
	for (i=0; i < num_packages; i++)
	    fprintf(yyout, "%s.o ", packages[i]);
	fprintf(yyout, "\n");
    }
    else if (!strncmp(name, "ppchar:", 7) && *(name + 8))
        ppchar = *(name + 8);
    else yyerrorp("Unidentified %cpragma");
}

static void
preprocess() {
    register char *yyp, *yyp2;
    int c;
    int cond;

    while (buffered ? (yyp = yyp2 = outp) : fgets(yyp = yyp2 = defbuf + (DEFMAX >> 1), MAXLINE-1, yyin)) {
	if (!buffered) current_line++;
	else buffered = 0;
	while (isspace(*yyp2)) yyp2++;
	if ((c = *yyp2) == ppchar){
	    int quote = 0;
	    char sp_buf = 0, *oldoutp;

	    if (c == '%' && yyp2[1] == '%')
		grammar_mode++;
	    outp = 0;
	    if (yyp != yyp2) yyerrorp("Misplaced '%c'.\n");
	    while (isspace(*++yyp2));
	    yyp++;
	    for (;;) {
		if ((c = *yyp2++) == '"') quote ^= 1;
		else{
		    if (!quote && c == '/'){
			if (*yyp2 == '*'){
			    yyp2 = skip_comment(yyp2, 0);
			    continue;
			}
			else if (*yyp2 == '/') break;
		    }
		    if (!outp && isspace(c)) outp = yyp;
		    if (c == '\n' || c == EOF) break;
		}
		*yyp++ = c;
	    }
	    
	    if (outp) {
		if (yyout) sp_buf = *(oldoutp = outp);
		*outp++ = 0;
		while (isspace(*outp)) outp++;
	    }
	    else outp = yyp;
	    *yyp = 0;
	    yyp = defbuf + (DEFMAX >> 1) + 1;

	    if (!strcmp("define", yyp)){
		handle_define();
	    } else if (!strcmp("if", yyp)) {
		cond = cond_get_exp(0);
		if (*outp != '\n') yyerrorp("Condition too complex in %cif");
		else handle_cond(cond);
	    } else if (!strcmp("ifdef", yyp)) {
		deltrail();
		handle_cond(lookup_define(outp) != 0);
	    } else if (!strcmp("ifndef", yyp)) {
		deltrail();
		handle_cond(!lookup_define(outp));
	    } else if (!strcmp("elif", yyp)) {
		handle_elif();
	    } else if (!strcmp("else", yyp)) {
		handle_else();
	    } else if (!strcmp("endif", yyp)) {
		handle_endif();
	    } else if (!strcmp("undef", yyp)) {
		defn_t *d;
		
		deltrail();
		if ((d = lookup_define(outp)))
		    d->flags |= DEF_IS_UNDEFINED;
	    } else if (!strcmp("echo", yyp)) {
		fprintf(stderr, "echo at line %d of %s: %s\n", current_line, current_file, outp);
	    } else if (!strcmp("include", yyp)) {
		handle_include(outp);
	    } else if (!strcmp("pragma", yyp)) {
		handle_pragma(outp);
	    } else if (yyout){
		if (!strcmp("line", yyp)){
		    fprintf(yyout, "#line %d \"%s\"\n", current_line,
			    current_file);
		} else {
		    if (sp_buf) *oldoutp = sp_buf;
		    if (pragmas & PRAGMA_NOTE_CASE_START){
			if (*yyp == '%') pragmas &= ~PRAGMA_NOTE_CASE_START;
		    }
		    fprintf(yyout, "%s\n", yyp-1);
		}
	    } else {
		char buff[200];
		sprintf(buff, "Unrecognised %c directive : %s\n", ppchar, yyp);
		yyerror(buff);
	    }
	}
	else if (c == '/'){
	    if ((c = *++yyp2) == '*'){
		if (yyout) fputs(yyp, yyout);
		yyp2 = skip_comment(yyp2, 1);
	    } else if (c == '/' && !yyout) continue;
	    else if (yyout){
		fprintf(yyout, "%s", yyp);
	    }
	}
	else if (yyout){
	    fprintf(yyout, "%s", yyp);
	    if (pragmas & PRAGMA_NOTE_CASE_START){
		static int line_to_print;
		
		line_to_print = 0;
		
		if (!in_c_case){
		    while (isalunum(*yyp2)) yyp2++;
		    while (isspace(*yyp2)) yyp2++;
		    if (*yyp2 == ':'){
			in_c_case = 1;
			yyp2++;
		    }
		}
		
		if (in_c_case){
		    while (c = *yyp2++){
			switch(c){
			  case '{':
			    {
				if (!cquote && (++block_nest == 1))
				    line_to_print = 1;
				break;
			    }
			    
			  case '}':
			    {
				if (!cquote){
				    if (--block_nest < 0) yyerror("Too many }'s");
				}
				break;
			    }
			    
			  case '"':
                            if (!(cquote & CHAR_QUOTE)) cquote ^= STRING_QUOTE;
                            break;
			    
			  case '\'':
                            if (!(cquote & STRING_QUOTE)) cquote ^= CHAR_QUOTE;
                            break;
			    
			  case '\\':
                            if (cquote && *yyp2) yyp2++;
                            break;
			    
			  case '/':
                            if (!cquote){
                                if ((c = *yyp2) == '*'){
                                    yyp2 = skip_comment(yyp2, 1);
                                } else if (c == '/'){
                                    *(yyp2-1) = '\n';
                                    *yyp2 = '\0';
                                }
                            }
                            break;
			    
			  case ':':
                            if (!cquote && !block_nest)
                                yyerror("Case started before ending previous case with ;");
                            break;
			    
			  case ';':
                            if (!cquote && !block_nest) in_c_case = 0;
			}
		    }
		}
		
		if (line_to_print)
		    fprintf(yyout, "#line %d \"%s\"\n", current_line + 1,current_file);

	    }
	}
    }
    if (iftop){
      ifstate_t *p = iftop;

      while (iftop){
          p = iftop;
          iftop = p->next;
          free(p);
      }
      yyerrorp("Missing %cendif");
    }
    fclose(yyin);
    free(current_file);
    nexpands = 0;
    if (inctop){
      incstate *p = inctop;

      current_file = p->file;
      current_line = p->line;
      yyin = p->yyin;
      inctop = p->next;
      free((char *) p);
      preprocess();
    } else yyout = 0;
}

void make_efun_tables()
{
#define NUM_FILES     5
    static char* outfiles[NUM_FILES] = { 
	EFUN_TABLE, OPC_PROF, OPCODES, EFUN_PROTO, EFUN_DEFS 
    };
    FILE *files[NUM_FILES];
    int i;

    for (i = 0; i < NUM_FILES; i++) {
	files[i] = fopen(outfiles[i], "w");
	if (!files[i]) {
	    fprintf(stderr, "make_func: unable to open %s\n", outfiles[i]);
	    exit(-1);
	}
	fprintf(files[i], 
		"/*\n\tThis file is automatically generated by make_func.\n");
	fprintf(files[i],
		"\tdo not make any manual changes to this file.\n*/\n\n");
    }
	
    fprintf(files[0], "\ntypedef void (*func_t) PROT((void));\n\n");
    fprintf(files[0],"func_t efun_table[] = {\n");

    fprintf(files[1],"\ntypedef struct opc_s { char *name; int count; } opc_t;\n\n");
    fprintf(files[1],"opc_t opc_efun[] = {\n");

    for (i = 0; i < (num_buff - 1); i++) {
	if (has_token[i]) {
	    fprintf(files[0],"\tf_%s,\n",key[i]);
	    fprintf(files[1],"{\"%s\", 0},\n",key[i]);
	    fprintf(files[3],"void f_%s PROT((void));\n", key[i]);
	}
    }
    fprintf(files[0],"\tf_%s};\n",key[num_buff - 1]);
    fprintf(files[1],"{\"%s\", 0}};\n",key[num_buff - 1]);
    fprintf(files[3],"void f_%s PROT((void));\n", key[i]);

    for (i = 0; i < num_buff; i++) {
	if (has_token[i])
	    fprintf(files[0],"void f_%s PROT((void));\n",key[i]);
    }

    fprintf(files[2], "\n/* operators */\n\n");
    for (i = 0; i < op_code; i++) {
	fprintf(files[2],"#define %-30s %d\n", oper_codes[i], i+1);
    }
    fprintf(files[2],"\n/* efuns */\n#define BASE %d\n\n", op_code+1);
    for (i = 0; i < efun_code; i++) {
	fprintf(files[2],"#define %-30s %d\n", efun_codes[i], i+op_code+1);
    }
    if (efun_code + op_code < 256) {
	fprintf(files[2],"#undef NEEDS_CALL_EXTRA\n");
    } else {
	fprintf(files[2],"#define NEEDS_CALL_EXTRA\n");
	if (efun_code + op_code > 510) {
	    fprintf(stderr, "You have way too many efuns.  Contact the MudOS developers if you really need this many.\n");
	}
    }
    fprintf(files[2],"\n/* efuns */\n#define NUM_OPCODES %d\n\n", efun_code + op_code);
    
    /* Now sort the main_list */
    for (i = 0; i < num_buff; i++) {
       int j;
       for (j = 0; j < i; j++)
	   if (strcmp(key[i], key[j]) < 0) {
	      char *tmp;
	      int tmpi;
	      tmp = key[i]; key[i] = key[j]; key[j] = tmp;
	      tmp = buf[i]; buf[i] = buf[j]; buf[j] = tmp;
	      tmpi = has_token[i];
	      has_token[i] = has_token[j]; has_token[j] = tmpi;
	   }
    }

    /* Now display it... */
    fprintf(files[4], "{\n");
    for (i = 0; i < num_buff; i++)
	fprintf(files[4], "%s", buf[i]);
    fprintf(files[4], "\n};\nint efun_arg_types[] = {\n");
    for (i=0; i < last_current_type; i++) {
	if (arg_types[i] == 0)
	    fprintf(files[4], "0,\n");
	else
	    fprintf(files[4], "%s,", ctype(arg_types[i]));
    }
    fprintf(files[4],"};\n");

    for (i=0; i < NUM_FILES; i++)
	fclose(files[i]);
}

static void handle_options() {
    open_input_file(OPTIONS_H);
    ppchar = '#';
    preprocess();
    create_option_defines();
}

static void handle_build_func_spec P1(char *, command) {
    char buf[1024];
    int i;

    sprintf(buf, "%s %s >%s", command, FUNC_SPEC, FUNC_SPEC_CPP);
    system(buf);
    for (i = 0; i < num_packages; i++) {
	sprintf(buf, "%s packages/%s_spec.c >>%s", 
		command, packages[i], FUNC_SPEC_CPP);
	system(buf);
    }
}

static void handle_process P1(char *, file) {
    char buf[1024];
    int l;

    strcpy(buf, file);
    l = strlen(buf);
    if (strcmp(buf + l - 4, ".pre")) {
	fprintf(stderr, "Filename for -process must end in .pre\n");
	exit(-1);
    }
    *(buf + l - 4) = 0;
    
    open_input_file(file);
    open_output_file(buf);
    ppchar = '%';
    preprocess();
    close_output_file();
}

static void handle_build_efuns() {
    num_buff = op_code = efun_code = 0;

    open_input_file(FUNC_SPEC_CPP);
    yyparse();
    make_efun_tables();
}

#ifdef SYSMALLOC
#  define THE_MALLOC "sysmalloc.c"
#endif
#ifdef SMALLOC
#  define THE_MALLOC "smalloc.c"
#endif
#ifdef BSDMALLOC
#  define THE_MALLOC "bsdmalloc.c"
#endif

#ifdef WRAPPEDMALLOC
#  define THE_WRAPPER "wrappedmalloc.c"
#endif

#ifdef DEBUGMALLOC
#  define THE_WRAPPER "debugmalloc.c"
#endif

static void handle_malloc() {
#if !defined(THE_MALLOC) && !defined(THE_WRAPPER)
    fprintf(stderr, "Memory package and/or malloc wrapper incorrectly specified in options.h\n");
    exit(-1);
#endif
    unlink("malloc.c");
    unlink("mallocwrapper.c");
#ifdef THE_WRAPPER
    printf("Using memory allocation package: %s\n\t\tWrapped with: %s\n",
	   THE_MALLOC, THE_WRAPPER);
    link(THE_WRAPPER, "mallocwrapper.c");
#else
    printf("Using memory allocation package: %s\n", THE_MALLOC);
    link("plainwrapper.c", "mallocwrapper.c");
#endif
    link(THE_MALLOC, "malloc.c");
}

static int check_include2 P4(char *, tag, char *, file,
			     char *, before, char *, after) {
    char buf[1024];
    FILE *ct;

    printf("Checking for include file <%s> ... ", file);
    ct = fopen("comptest.c", "w");
    fprintf(ct, "#include <sys/types.h>\n%s\n#include <%s>\n%s\n", 
	    before, file, after);
    fclose(ct);
    
#ifdef DEBUG
    sprintf(buf, "%s %s -c comptest.c", COMPILER, CFLAGS);
#else
    sprintf(buf, "%s %s -c comptest.c >/dev/null 2>&1", COMPILER, CFLAGS);
#endif
    if (!system(buf)) {
	fprintf(yyout, "#define %s\n", tag);
	printf("exists\n");
	return 1;
    }
    printf("does not exist or is unusable\n");
    return 0;
}

static int check_include P2(char *, tag, char *, file) {
    char buf[1024];
    FILE *ct;

    printf("Checking for include file <%s> ... ", file);
    ct = fopen("comptest.c", "w");
    fprintf(ct, "#include \"std_incl.h\"\n#include <%s>\n", file);
    fclose(ct);
    
#ifdef DEBUG
    sprintf(buf, "%s %s -c comptest.c", COMPILER, CFLAGS);
#else
    sprintf(buf, "%s %s -c comptest.c >/dev/null 2>&1", COMPILER, CFLAGS);
#endif
    if (!system(buf)) {
	fprintf(yyout, "#define %s\n", tag);
	/* Make sure the define exists for later checks */
	fflush(yyout);
	printf("exists\n");
	return 1;
    }
    printf("does not exist\n");
    return 0;
}

static int check_library P1(char *, lib) {
    char buf[1024];
    FILE *ct;

    printf("Checking for library %s ... ", lib);
    ct = fopen("comptest.c", "w");
    fprintf(ct, "int main() { exit(0); }");
    fclose(ct);
    
#ifdef DEBUG
    sprintf(buf, "%s %s comptest.c %s", COMPILER, CFLAGS, lib);
#else
    sprintf(buf, "%s %s comptest.c %s >/dev/null 2>&1", COMPILER, CFLAGS, lib);
#endif
    if (!system(buf)) {
	fprintf(yyout, " %s", lib);
	printf("exists\n");
	return 1;
    }
    printf("does not exist\n");
    return 0;
}

static int check_ret_type P4(char *, tag, char *, pre,
			     char *, type, char *, func) {
    char buf[1024];
    FILE *ct;

    printf("Checking return type of %s() ...", func);
    ct = fopen("comptest.c", "w");
    fprintf(ct, "%s\n\n%s%s();\n", pre, type, func);
    fclose(ct);
    
    sprintf(buf, "%s %s -c comptest.c >/dev/null 2>&1", COMPILER, CFLAGS);
    if (!system(buf)) {
	fprintf(yyout, "#define %s\n", tag);
	printf("returns %s\n", type);
	return 1;
    }
    printf("does not return %s\n", type);
    return 0;
}

static int check_prog P3(char *, tag, char *, pre, char *, code) {
    char buf[1024];
    FILE *ct;

    ct = fopen("comptest.c", "w");
    fprintf(ct, "%s\n\nint main() {%s}\n", pre, code);
    fclose(ct);
    
    sprintf(buf, "%s %s comptest.c -o comptest >/dev/null 2>&1", COMPILER, CFLAGS);
    if (!system(buf)) {
	fprintf(yyout, "#define %s\n", tag);
	return 1;
    }
    return 0;
}

static void handle_configure() {
    open_output_file("configure.h");
    check_include("INCL_STDLIB_H", "stdlib.h");
    check_include("INCL_UNISTD_H", "unistd.h");
    check_include("INCL_TIME_H", "time.h");
    check_include("INCL_SYS_TIMES_H", "sys/times.h");
    check_include("INCL_FCNTL_H", "fcntl.h");
    check_include("INCL_SYS_TIME_H", "sys/time.h");
    check_include("INCL_DOS_H", "dos.h");
    check_include("INCL_USCLKC_H", "usclkc.h");

    check_include("INCL_NETINET_IN_H", "netinet/in.h");
    check_include("INCL_ARPA_INET_H", "arpa/inet.h");

    check_include("INCL_SYS_IOCTL_H", "sys/ioctl.h");
    check_include("INCL_SYS_SOCKET_H", "sys/socket.h");
    check_include("INCL_NETDB_H", "netdb.h");
    /* TELOPT_NAWS is missing from <arpa/telnet.h> on some systems */
    check_include("INCL_ARPA_TELNET_H", "arpa/telnet.h", "", "int i=TELOPT_MAWS;");
    check_include("INCL_SYS_SEMA_H", "sys/sema.h");
    check_include("INCL_SYS_SOCKETVAR_H", "sys/socketvar.h");
    check_include("INCL_SOCKET_H", "socket.h");
    check_include("INCL_RESOLVE_H", "resolve.h");

    check_include("INCL_SYS_STAT_H", "sys/stat.h");

    /* sys/dir.h is BSD, dirent is sys V */
    if (check_include("INCL_DIRENT_H", "dirent.h")) {
	check_include("INCL_SYS_DIRENT_H", "sys/dirent.h");
	check_ret_type("USE_STRUCT_DIRENT", 
		       "#include <dirent.h>", "struct dirent *", "readdir");
    } else {
	check_include("INCL_SYS_DIR_H", "sys/dir.h");
    }

    check_include("INCL_SYS_FILIO_H", "sys/filio.h");
    check_include("INCL_SYS_SOCKIO_H", "sys/sockio.h");
    check_include("INCL_SYS_MKDEV_H", "sys/mkdev.h");
    check_include("INCL_SYS_RESOURCE_H", "sys/resource.h");
    check_include("INCL_SYS_RUSAGE_H", "sys/rusage.h");
    check_include("INCL_SYS_CRYPT_H", "sys/crypt.h");

    if (!check_prog("DRAND48", "#include <math.h>", "srand48(0);"))
	if (!check_prog("RAND", "#include <math.h>", "srand(0);"))
	    if (!check_prog("RANDOM", "#include <math.h>", "srandom(0);"))
		printf("WARNING: did not find a random number generator\n");

    check_prog("HAS_UALARM", "", "ualarm(0, 0);");

    fprintf(yyout, "#define SIZEOF_INT %i\n", sizeof(int));
    fprintf(yyout, "#define SIZEOF_PTR %i\n", sizeof(char *));
    fprintf(yyout, "#define SIZEOF_SHORT %i\n", sizeof(short));
    fprintf(yyout, "#define SIZEOF_FLOAT %i\n", sizeof(float));

    close_output_file();

    open_output_file("system_libs");
    fprintf(yyout, "LIBS=");
    check_library("-lresolv");
    check_library("-lbsd");
    check_library("-lBSD");
    check_library("-ly");
    check_library("-lcrypt");
    check_library("-lsocket");
    check_library("-linet");
    check_library("-lnsl");
    check_library("-lnsl_s");
    check_library("-lseq");
    check_library("-lmalloc");
    check_library("-lm");
    fprintf(yyout, "\n\n");
    close_output_file();
}

int main P2(int, argc, char **, argv) {
    int idx = 1;

    while (idx < argc) {
	if (argv[idx][0] != '-') {
	    fprintf(stderr, SYNTAX);
	    exit(-1);
	}
	if (strcmp(argv[idx], "-configure")==0) {
	    handle_configure();
	} else
	if (strcmp(argv[idx], "-process")==0) {
	    handle_process(argv[++idx]);
	} else
	if (strcmp(argv[idx], "-options")==0) {
	    handle_options();
	} else
	if (strcmp(argv[idx], "-malloc")==0) {
	    handle_malloc();
	} else
	if (strcmp(argv[idx], "-build_func_spec")==0) {
	    handle_build_func_spec(argv[++idx]);
	} else
	if (strcmp(argv[idx], "-build_efuns")==0) {
	    handle_build_efuns();
	} else {
	    fprintf(stderr, "Unrecognized flag %s\n", argv[idx]);
	    exit(-1);
	}
	idx++;
    }
    return 0;
}
