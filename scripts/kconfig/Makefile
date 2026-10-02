# 精简 kconfig 构建：make → conf + mconf
HOSTCC ?= gcc
CFLAGS  = -O2 -I. -I./lxdialog -DCONFIG_=\"CONFIG_\"
LXD     = lxdialog/checklist.o lxdialog/inputbox.o lxdialog/menubox.o \
          lxdialog/textbox.o lxdialog/util.o lxdialog/yesno.o
COMMON  = confdata.o expr.o symbol.o menu.o util.o preprocess.o

conf: conf.o $(COMMON) lexer.o parser.o
	$(HOSTCC) $^ -o $@

mconf: mconf.o $(COMMON) lexer.o parser.o $(LXD)
	$(HOSTCC) $^ -o $@ -lncursesw -lmenuw -lpanelw

%.o: %.c
	$(HOSTCC) $(CFLAGS) -c $< -o $@

# flex/bison 生成词法/语法分析器（buildroot 2022.11 命名为 lexer.l / parser.y）
lexer.o: lexer.l parser.y
	flex -o lex.zconf.c lexer.l
	bison -l -o parser.tab.c parser.y
	$(HOSTCC) $(CFLAGS) -c lex.zconf.c -o lexer.o -x c
	$(HOSTCC) $(CFLAGS) -I. -c parser.tab.c -o parser.o

clean:
	rm -f *.o lxdialog/*.o conf mconf lex.zconf.c parser.tab.c parser.tab.h
