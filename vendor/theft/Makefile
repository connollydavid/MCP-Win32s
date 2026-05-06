PROJECT = 	theft
BUILD =		build
SRC =		src
TEST =		test
INC =		inc
SCRIPTS =	scripts
VENDOR =	vendor
COVERAGE =	-fprofile-arcs -ftest-coverage
PROFILE =	-pg
OPTIMIZE = 	-O3
#OPTIMIZE = 	-O0 ${COVERAGE}
#OPTIMIZE = 	-O0 ${PROFILE}

WARN = 		-Wall -Wextra -pedantic
CDEFS +=	-D_POSIX_C_SOURCE=199309L -D_C99_SOURCE
CINCS += 	-I${INC} -I${VENDOR} -I${BUILD}
CFLAGS += 	-std=c99 -g ${WARN} ${CDEFS} ${OPTIMIZE} ${CINCS}
CFLAGS +=	-fPIC

# Note: -lm is only needed if using built-in floating point generators
LDFLAGS +=	-lm

all: ${BUILD}/lib${PROJECT}.a
all: ${BUILD}/test_${PROJECT}

TEST_CFLAGS += 	${CFLAGS} -I${SRC}
TEST_LDFLAGS +=	${LDFLAGS}

OBJS= 		${BUILD}/theft.o \
		${BUILD}/theft_autoshrink.o \
		${BUILD}/theft_bloom.o \
		${BUILD}/theft_call.o \
		${BUILD}/theft_hash.o \
		${BUILD}/theft_random.o \
		${BUILD}/theft_rng.o \
		${BUILD}/theft_run.o \
		${BUILD}/theft_shrink.o \
		${BUILD}/theft_trial.o \
		${BUILD}/theft_aux.o \
		${BUILD}/theft_aux_builtin.o \

TEST_OBJS=	${BUILD}/test_theft.o \
		${BUILD}/test_theft_autoshrink.o \
		${BUILD}/test_theft_autoshrink_ll.o \
		${BUILD}/test_theft_autoshrink_bulk.o \
		${BUILD}/test_theft_autoshrink_int_array.o \
		${BUILD}/test_theft_aux.o \
		${BUILD}/test_theft_bloom.o \
		${BUILD}/test_theft_error.o \
		${BUILD}/test_theft_prng.o \
		${BUILD}/test_theft_integration.o \
		${BUILD}/test_char_array.o \


# C89 / C99 build variants (use separate build dirs to avoid stale .o files)
C89_BUILD =	build-c89
C89_CDEFS =	-D_POSIX_C_SOURCE=199309L
C89_CFLAGS =	-std=c89 -Wpedantic -Wno-long-long -Wno-variadic-macros -g -Wall -Wextra -pedantic ${C89_CDEFS} ${OPTIMIZE} -I${INC} -I${VENDOR} -I${C89_BUILD} -fPIC
C89_TEST_CFLAGS =	${C89_CFLAGS} -I${SRC}
C89_OBJS =	$(patsubst ${BUILD}/%.o,${C89_BUILD}/%.o,${OBJS})
C89_TEST_OBJS =	$(patsubst ${BUILD}/%.o,${C89_BUILD}/%.o,${TEST_OBJS})

C99_BUILD =	build-c99
C99_CDEFS =	-D_POSIX_C_SOURCE=199309L -D_C99_SOURCE
C99_CFLAGS =	-std=c99 -g -Wall -Wextra -pedantic ${C99_CDEFS} ${OPTIMIZE} -I${INC} -I${VENDOR} -I${C99_BUILD} -fPIC
C99_TEST_CFLAGS =	${C99_CFLAGS} -I${SRC}
C99_OBJS =	$(patsubst ${BUILD}/%.o,${C99_BUILD}/%.o,${OBJS})
C99_TEST_OBJS =	$(patsubst ${BUILD}/%.o,${C99_BUILD}/%.o,${TEST_OBJS})

c89: ${C89_BUILD}/lib${PROJECT}.a ${C89_BUILD}/test_${PROJECT}
c99: ${C99_BUILD}/lib${PROJECT}.a ${C99_BUILD}/test_${PROJECT}

# Basic targets

test: ${BUILD}/test_${PROJECT}
	${BUILD}/test_${PROJECT} ${ARG}

clean:
	rm -rf ${BUILD} ${C89_BUILD} ${C99_BUILD} gmon.out cscope.out

cscope: ${SRC}/*.c ${SRC}/*.h ${INC}/*
	cscope -bu ${SRC}/*.[ch] ${INC}/*.h ${TEST}/*.[ch]

ctags: ${BUILD}/tags

tags: ${BUILD}/TAGS

${BUILD}/lib${PROJECT}.a: ${OBJS} ${BUILD}/lib${PROJECT}.pc
	ar -rcs ${BUILD}/lib${PROJECT}.a ${OBJS}

${BUILD}/test_${PROJECT}: ${OBJS} ${TEST_OBJS}
	${CC} -o $@ ${OBJS} ${TEST_OBJS} ${TEST_CFLAGS} ${TEST_LDFLAGS}

${BUILD}/%.o: ${SRC}/%.c ${SRC}/*.h ${INC}/* | ${BUILD}
	${CC} -c -o $@ ${CFLAGS} $<

${BUILD}/%.o: ${TEST}/%.c ${SRC}/*.h ${INC}/* | ${BUILD}
	${CC} -c -o $@ ${TEST_CFLAGS} $<

${BUILD}/tags: ${SRC}/*.c ${SRC}/*.h ${INC}/* | ${BUILD}
	ctags -f $@ \
		--tag-relative --langmap=c:+.h --fields=+l --c-kinds=+l --extra=+q \
		${SRC}/*.[ch] ${INC}/*.h ${TEST}/*.[ch]

${BUILD}/TAGS: ${SRC}/*.c ${SRC}/*.h ${INC}/* | ${BUILD}
	etags -o $@ ${SRC}/*.[ch] ${INC}/*.h ${TEST}/*.[ch]

${BUILD}/*.o: Makefile

${BUILD}:
	mkdir ${BUILD}

${BUILD}/cover: | ${BUILD}
	mkdir ${BUILD}/cover

profile: test
	gprof build/test_theft

coverage: test | ${BUILD} ${BUILD}/cover
	ls -1 src/*.c | sed -e "s#src/#build/#" | xargs -n1 gcov
	@echo moving coverage files to ${BUILD}/cover
	mv *.gcov ${BUILD}/cover

${BUILD}/theft_bloom.o: ${BUILD}/bits_lut.h
${BUILD}/theft_autoshrink.o: ${BUILD}/bits_lut.h

${BUILD}/bits_lut.h: | ${BUILD}
	${SCRIPTS}/mk_bits_lut > $@

${BUILD}/%.pc: pc/%.pc.in | ${BUILD}
	sed -e 's,@prefix@,${PREFIX},g' $< > $@

# C89 build rules
${C89_BUILD}/lib${PROJECT}.a: ${C89_OBJS}
	ar -rcs $@ ${C89_OBJS}

${C89_BUILD}/test_${PROJECT}: ${C89_OBJS} ${C89_TEST_OBJS}
	${CC} -o $@ ${C89_OBJS} ${C89_TEST_OBJS} ${C89_TEST_CFLAGS} ${LDFLAGS}

${C89_BUILD}/%.o: ${SRC}/%.c ${SRC}/*.h ${INC}/* | ${C89_BUILD}
	${CC} -c -o $@ ${C89_CFLAGS} $<

${C89_BUILD}/%.o: ${TEST}/%.c ${SRC}/*.h ${INC}/* | ${C89_BUILD}
	${CC} -c -o $@ ${C89_TEST_CFLAGS} $<

${C89_BUILD}/theft_bloom.o: ${C89_BUILD}/bits_lut.h
${C89_BUILD}/theft_autoshrink.o: ${C89_BUILD}/bits_lut.h

${C89_BUILD}/bits_lut.h: | ${C89_BUILD}
	${SCRIPTS}/mk_bits_lut > $@

${C89_BUILD}:
	mkdir ${C89_BUILD}

# C99 build rules
${C99_BUILD}/lib${PROJECT}.a: ${C99_OBJS}
	ar -rcs $@ ${C99_OBJS}

${C99_BUILD}/test_${PROJECT}: ${C99_OBJS} ${C99_TEST_OBJS}
	${CC} -o $@ ${C99_OBJS} ${C99_TEST_OBJS} ${C99_TEST_CFLAGS} ${LDFLAGS}

${C99_BUILD}/%.o: ${SRC}/%.c ${SRC}/*.h ${INC}/* | ${C99_BUILD}
	${CC} -c -o $@ ${C99_CFLAGS} $<

${C99_BUILD}/%.o: ${TEST}/%.c ${SRC}/*.h ${INC}/* | ${C99_BUILD}
	${CC} -c -o $@ ${C99_TEST_CFLAGS} $<

${C99_BUILD}/theft_bloom.o: ${C99_BUILD}/bits_lut.h
${C99_BUILD}/theft_autoshrink.o: ${C99_BUILD}/bits_lut.h

${C99_BUILD}/bits_lut.h: | ${C99_BUILD}
	${SCRIPTS}/mk_bits_lut > $@

${C99_BUILD}:
	mkdir ${C99_BUILD}

# Installation
PREFIX ?=	/usr/local
PKGCONFIG_DST ?=${DESTDIR}${PREFIX}/lib/pkgconfig
INSTALL ?= 	install
RM ?=		rm

install: ${BUILD}/lib${PROJECT}.a ${BUILD}/lib${PROJECT}.pc
	${INSTALL} -d ${DESTDIR}${PREFIX}/lib/
	${INSTALL} -c ${BUILD}/lib${PROJECT}.a ${DESTDIR}${PREFIX}/lib/lib${PROJECT}.a
	${INSTALL} -d ${DESTDIR}${PREFIX}/include/
	${INSTALL} -c ${INC}/${PROJECT}.h ${DESTDIR}${PREFIX}/include/
	${INSTALL} -c ${INC}/${PROJECT}_types.h ${DESTDIR}${PREFIX}/include/
	${INSTALL} -d ${DESTDIR}${PKGCONFIG_DST}
	${INSTALL} -c ${BUILD}/lib${PROJECT}.pc ${DESTDIR}${PKGCONFIG_DST}/

uninstall:
	${RM} ${DESTDIR}${PREFIX}/lib/lib${PROJECT}.a
	${RM} ${DESTDIR}${PREFIX}/include/${PROJECT}.h
	${RM} ${DESTDIR}${PREFIX}/include/${PROJECT}_types.h
	${RM} ${DESTDIR}${PKGCONFIG_DST}/lib${PROJECT}.pc


# Other dependencies
${BUILD}/theft.o: Makefile ${INC}/*.h

.PHONY: all test clean cscope ctags tags coverage profile install uninstall c89 c99
