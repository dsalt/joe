include $(top_srcdir)/Makevars

data_lang_DATA = ru.po de.po fr.po uk.po zh_TW.po en_GB.po

EXTRA_DIST = $(data_lang_DATA)

XGETTEXT = xgettext
MSGMERGE_UPDATE = msgmerge --update

all: $(data_lang_DATA)

AM_V_XGETTEXT = $(am__v_XGETTEXT_@AM_V@)
am__v_XGETTEXT_ = $(am__v_XGETTEXT_@AM_DEFAULT_V@)
am__v_XGETTEXT_0 = @echo "  GETTEXT " $@;
am__v_XGETTEXT_1 =

# let's just assume that we have xgettext 0.17 or later
joe.pot-update: joerc.for-po
	$(AM_V_XGETTEXT)$(XGETTEXT) --keyword=_ --from-code=UTF-8 \
		--default-domain=joe --directory=$(top_srcdir) \
		--add-comments=TRANSLATORS: \
		--copyright-holder='Copyright (C) 1988-2026 Joe Allen and contributors' \
		--package-name=joe \
		--package-version="`sed -re 's/^AC_INIT\(\[joe],\[(.*)]\)$$/\1/; t; d' $(top_srcdir)/configure.ac`" \
		--msgid-bugs-address=https://github.com/joe-editor/joe/issues \
		$(foreach file,$(wildcard ../joe/*.c ../joe/*.h),$(patsubst ../%,%,$(file)))
	$(AM_V_at)mv joe.po $(srcdir)/joe.pot

$(srcdir)/joe.pot:
	$(MAKE) joe.pot-update

$(data_lang_DATA): $(srcdir)/joe.pot $(srcdir)/joerc.sed
	@if ! $(AM_V_P); then echo " UPDATE  " $@; fi
	$(AM_V_at)lang=`echo $@ | sed -e 's,.*/,,' -e 's/\.po$$//'`; \
	if test -f "$(srcdir)/$${lang}.po"; then \
	  test "$(srcdir)" = . && cdcmd="" || cdcmd="cd $(srcdir) && "; \
	  cd $(srcdir) && $(MSGMERGE_UPDATE) $${lang}.po joe.pot; \
	else $(MAKE) $${lang}.po-create; \
	fi

%.po-create:
	@lang=`echo $@ | sed -e 's/\.po-create$$//'`; \
	echo "*** File $$lang.po does not exist." >&2; \
	echo "*** If you are a translator, you can create it through 'msginit'." >&2; \
	exit 1
