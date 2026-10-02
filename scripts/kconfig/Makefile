# kconfig 精简构建（适配 buildroot 2022.11 老式 kconfig，2026-10-02 实测通过）
# 关键：zconf.tab.c_shipped 把 util/confdata/expr/symbol/menu 等 .c 全部
# include 成单一编译单元，所以只需 conf.o + zconf.tab.o 两个对象；
# 词法文件用预生成的 zconf.lex.c_shipped，无需 flex/bison。
HOSTCC ?= gcc
CFLAGS  = -O2 -I. -DCONFIG_=\"CONFIG_\" -include limits.h -include ctype.h -include errno.h -DLOCALE
CURSES  = -DCURSES_LOC='<ncursesw/curses.h>' 
LXD     = lxdialog/checklist.o lxdialog/util.o lxdialog/inputbox.o \
          lxdialog/textbox.o lxdialog/yesno.o lxdialog/menubox.o

conf: conf.o zconf.tab.o
	$(HOSTCC) $^ -o $@

mconf: mconf.o zconf.tab.o $(LXD)
	$(HOSTCC) $^ -o $@ -lncursesw -lmenuw -lpanelw   # 原来是 -lncurses -lmenu -lpanel

zconf.tab.c: zconf.tab.c_shipped
	cat $< > $@

zconf.lex.c: zconf.lex.c_shipped
	cat $< > $@

zconf.tab.o: zconf.tab.c zconf.lex.c
	$(HOSTCC) $(CFLAGS) -c $< -o $@

%.o: %.c
	$(HOSTCC) $(CFLAGS) -c $< -o $@

mconf.o: mconf.c
	$(HOSTCC) $(CFLAGS) $(CURSES) -c $< -o $@

lxdialog/%.o: lxdialog/%.c
	$(HOSTCC) $(CFLAGS) $(CURSES) -c $< -o $@

clean:
	rm -f *.o lxdialog/*.o conf mconf zconf.tab.c zconf.lex.c
