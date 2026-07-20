/* freestanding <ctype.h> */
#ifndef PC64_CTYPE_H
#define PC64_CTYPE_H
static inline int isspace(int c){return c==32||(c>=9&&c<=13);}
static inline int isdigit(int c){return c>=48&&c<=57;}
static inline int isalpha(int c){return (c>=65&&c<=90)||(c>=97&&c<=122);}
static inline int isalnum(int c){return isalpha(c)||isdigit(c);}
static inline int isupper(int c){return c>=65&&c<=90;}
static inline int islower(int c){return c>=97&&c<=122;}
static inline int isxdigit(int c){return isdigit(c)||(c>=65&&c<=70)||(c>=97&&c<=102);}
static inline int isprint(int c){return c>=32&&c<127;}
static inline int iscntrl(int c){return c<32||c==127;}
static inline int ispunct(int c){return isprint(c)&&!isalnum(c)&&c!=32;}
static inline int tolower(int c){return isupper(c)?c+32:c;}
static inline int toupper(int c){return islower(c)?c-32:c;}
#endif
