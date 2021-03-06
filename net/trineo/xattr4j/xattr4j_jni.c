/*
 * Created: 190313
 *
 * JNI wrappers of extended attribute syscalls
 *
 * see:
 *  xdprof.sourceforge.net/doxygen/jni_8h-source.html
 *  xdprof.sourceforge.net/doxygen/jni__md_8h-source.html
 *  docs.oracle.com/javase/8/docs/technotes/guides/jni/spec/functions.html
 *  ptolemy.berkeley.edu/~johnr/tutorials/assertions.html
 *  stackoverflow.com/questions/12207941/proper-way-to-clean-up-new-object-array-in-jni
 */

#include <jni.h>
#include <stdio.h>
#include <assert.h>         /* -DNDEBUG to disable runtime assertion */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/xattr.h>

#include "xattr4j_jni.h"

/* Used to indicate unused function parameters */
#define UNUSED(arg, ...)        (void) ((void) (arg), ##__VA_ARGS__)

/**
 * Compile-time assertion  see: linux/arch/x86/boot/boot.h
 */
#ifdef DEBUG
#define BUILD_BUG_ON(cond)      UNUSED(sizeof(char[-!(cond)]))
#else
#define BUILD_BUG_ON(cond)      UNUSED(cond)
#endif

#ifdef DEBUG
#define LOG(fmt, ...) fprintf(stderr, "xattr4j: " fmt "\n", ##__VA_ARGS__)
#else
#define LOG(fmt, ...) UNUSED(fmt, ##__VA_ARGS__)
#endif

#define assert_nonnull(p)       assert(p != NULL)

/**
 * Strong version of atomic compare-and-swap
 * @p           pointer to cas with
 * @o           old value
 * @n           new value
 * @return      true if success  false o.w.
 * see: http://donghao.org/2015/01/30/128bit-atomic-operation-in-arm64
 *
 * NOTE: GCC-compatible available only
 */
#define atomic_cas(p, o, n) ({      \
    __typeof(*(p)) t = (o);         \
    __atomic_compare_exchange_n(    \
        p, &t, n, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); \
})

static int __k = 0;
#define spin_lock()     while (!atomic_cas(&__k, 0, 1)) continue
#define spin_unlock()   do {            \
    int ok = atomic_cas(&__k, 1, 0);    \
    assert(ok);                         \
} while (0)

#define EXC_BUFSZ   8192
static char exc_buff[EXC_BUFSZ];

/**
 * Throw an exception
 * @env     JNI environment
 * @cls     Exception class
 * @fmt     Exception message format
 * @...     Exception message parameters
 */
static void throw_exc(JNIEnv *env, jclass cls, const char *fmt, ...)
{
    int sz;
    jint e;
    va_list ap;

    va_start(ap, fmt);
    spin_lock();
    /*
     * We use static buffer simply :. the exception itself may caused by OOM
     *  in such case further malloc(3) will fail again
     */
    sz = vsnprintf(exc_buff, EXC_BUFSZ, fmt, ap);

    e = (*env)->ThrowNew(env, cls, sz > 0 ? exc_buff : fmt);
    spin_unlock();
    va_end(ap);

    /* We should fail if exception cannot be throw */
    assert(e == 0);
}

#define throw_ioexc(env, fmt, ...) \
    throw_exc(env, java_io_IOException, fmt " line: %d", ##__VA_ARGS__, __LINE__)

/**
 * Get a NUL-terminated native C string from Java byte[]
 * @return      C char array    NULL if OOM(errno will set)
 *              Must be free via free(3)
 */
static inline char *get_cstr_bytes(JNIEnv *env, jbyteArray jbarr)
{
    char *buff = NULL;
    jbyte *arr;
    jsize sz;

    assert_nonnull(env);
    assert_nonnull(jbarr);

    arr = (*env)->GetByteArrayElements(env, jbarr, NULL);
    if (arr == NULL) {
        /* Set a pseudo-errno for JNIEnv->GetByteArrayElements() */
        errno = EIO;
        goto out;
    }

    sz = (*env)->GetArrayLength(env, jbarr);
    /* ENOMEM is the only possible errno from malloc(3) */
    buff = (char *) malloc(sz + 1);
    if (buff != NULL) {
        (void) memcpy(buff, arr, sz);
        buff[sz] = '\0';
    }

    (*env)->ReleaseByteArrayElements(env, jbarr, arr, JNI_ABORT);

out:
    return buff;
}

static jclass java_lang_String;
static jclass java_io_IOException;

/**
 * Initialize non-direct JNI functionalities
 *
 * see: javap -s java.lang.String
 */
JNIEXPORT void JNICALL
Java_net_trineo_xattr4j_XAttr4J_init(
        JNIEnv *env,
        jclass cls)
{
    BUILD_BUG_ON(sizeof(char) == sizeof(jbyte));
    BUILD_BUG_ON(sizeof(int) == sizeof(jint));
    BUILD_BUG_ON(sizeof(int) == sizeof(jsize));
    BUILD_BUG_ON(sizeof(ssize_t) <= sizeof(jlong));

    java_lang_String = (*env)->FindClass(env, "java/lang/String");
    /* JNIEnv->FindClass() will throw an exception if given class not found */
    if (java_lang_String == NULL) return;
    java_lang_String = (*env)->NewGlobalRef(env, java_lang_String);
    assert_nonnull(java_lang_String);

    java_io_IOException = (*env)->FindClass(env, "java/io/IOException");
    if (java_io_IOException == NULL) {
        (*env)->DeleteGlobalRef(env, java_lang_String);
        return;
    }
    java_io_IOException = (*env)->NewGlobalRef(env, java_io_IOException);
    if (java_io_IOException == NULL) {
        /* Properly drop global reference of java.lang.String before assert */
        (*env)->DeleteGlobalRef(env, java_lang_String);
    }
    assert_nonnull(java_io_IOException);
}

/**
 * XXX: When xattr data sized zero  we should return new byte[0] instead of null
 * @return      A valid(nonnull) byte[] array
 * @throws      IOException if internal failure
 */
JNIEXPORT jbyteArray JNICALL
Java_net_trineo_xattr4j_XAttr4J__1getxattr(
        JNIEnv *env,
        jclass cls,
        jbyteArray jbpath,
        jbyteArray jbname,
        jint options)
{
    jbyteArray out = NULL;
    char *path;
    char *name;
    jbyte *buff;
    ssize_t len;
    ssize_t len2;

    path = get_cstr_bytes(env, jbpath);
    if (path == NULL) {
        throw_ioexc(env, "get_cstr_bytes() path fail  errno: %d", errno);
        goto out1;
    }

    name = get_cstr_bytes(env, jbname);
    if (name == NULL) {
        throw_ioexc(env, "get_cstr_bytes() name fail  errno: %d", errno);
        goto out2;
    }

out_replay:
    len = getxattr(path, name, NULL, 0, 0, options);
    if (len < 0) {
        throw_ioexc(env, "getxattr(2) fail  errno: %d options: %#x name: %s path: %s", errno, options, name, path);
        goto out3;
    }

    /*
     * malloc(3) with zero-size have implementation-defined behaviour
     * BSD malloc(3) won't fail in such case
     * see: Open Group Base Specifications - malloc(3)
     */
    if (len != 0) {
        buff = (jbyte *) malloc(len);
        if (buff == NULL) {
            throw_ioexc(env, "malloc(3) fail  errno: %d len: %zd name: %s path: %s", errno, len, name, path);
            goto out3;
        }
    } else {
        buff = NULL;
    }

    len2 = getxattr(path, name, buff, len, 0, options);
    if (len2 < 0) {
        if (errno == ERANGE) {
            free(buff);
            LOG("TOCTTOU BUG in getxattr(2)  errno: %d len: %zd name: %s path: %s", errno, len, name, path);
            goto out_replay;
        }

        throw_ioexc(env, "getxattr(2) fail  errno: %d options: %#x len: %zd name: %s path: %s", errno, options, len, name, path);
        goto out4;
    }

    len = len2;     /* NOTE: 0 <= len2 <= len */

    /* zero is a valid size in JVM allocator */
    out = (*env)->NewByteArray(env, len);
    if (out != NULL) {
        (*env)->SetByteArrayRegion(env, out, 0, len, buff);
    } else {
        throw_ioexc(env, "JNIEnv->NewByteArray() fail  errno: %d len: %zd name: %s path: %s", errno, len, name, path);
    }

out4:
    free(buff);
out3:
    free(name);
out2:
    free(path);
out1:
    return out;
}

JNIEXPORT jbyteArray JNICALL
Java_net_trineo_xattr4j_XAttr4J__1fgetxattr(
        JNIEnv *env,
        jclass cls,
        jint fd,
        jbyteArray bname,
        jint options)
{
    jbyteArray out = NULL;
    char *name;
    jbyte *buff;
    ssize_t len;
    ssize_t len2;

    name = get_cstr_bytes(env, bname);
    if (name == NULL) {
        throw_ioexc(env, "get_cstr_bytes() name fail  errno: %d", errno);
        goto out1;
    }

out_replay:
    len = fgetxattr(fd, name, NULL, 0, 0, options);
    if (len < 0) {
        throw_ioexc(env, "fgetxattr(2) fail  errno: %d fd: %d name: %s options: %#x", errno, fd, name, options);
        goto out2;
    }

    /*
     * malloc(3) with zero-size have implementation-defined behaviour
     * BSD malloc(3) won't fail in such case
     * see: Open Group Base Specifications - malloc(3)
     */
    if (len != 0) {
        buff = (jbyte *) malloc(len);
        if (buff == NULL) {
            throw_ioexc(env, "malloc(3) fail  errno: %d fd: %d name: %s len: %zd", errno, fd, name, len);
            goto out2;
        }
    } else {
        buff = NULL;
    }

    len2 = fgetxattr(fd, name, buff, len, 0, options);
    if (len2 < 0) {
        if (errno == ERANGE) {
            free(buff);
            LOG("TOCTTOU BUG in fgetxattr(2)  errno: %d fd: %d name: %s len: %zd", errno, fd, name, len);
            goto out_replay;
        }

        throw_ioexc(env, "fgetxattr(2) fail  errno: %d fd: %d name: %s len: %zd options: %#x", errno, fd, name, len, options);
        goto out3;
    }

    len = len2;     /* NOTE: 0 <= len2 <= len */

    /* zero is a valid size in JVM allocator */
    out = (*env)->NewByteArray(env, len);
    if (out != NULL) {
        (*env)->SetByteArrayRegion(env, out, 0, len, buff);
    } else {
        throw_ioexc(env, "JNIEnv->NewByteArray() fail  errno: %d fd: %d name: %s len: %zd", errno, fd, name, len);
    }

out3:
    free(buff);
out2:
    free(name);
out1:
    return out;
}

JNIEXPORT void JNICALL
Java_net_trineo_xattr4j_XAttr4J__1setxattr(
        JNIEnv *env,
        jclass cls,
        jbyteArray jbpath,
        jbyteArray jbname,
        jbyteArray jbvalue,
        jint options)
{
    char *path;
    char *name;
    jbyte *value;
    jsize sz;

    path = get_cstr_bytes(env, jbpath);
    if (path == NULL) {
        throw_ioexc(env, "get_cstr_bytes() path fail  errno: %d", errno);
        return;
    }

    name = get_cstr_bytes(env, jbname);
    if (name == NULL) {
        throw_ioexc(env, "get_cstr_bytes() name fail  errno: %d", errno);
        goto out1;
    }

    value = (*env)->GetByteArrayElements(env, jbvalue, NULL);
    if (value == NULL) {
        throw_ioexc(env, "JNIEnv->GetByteArrayElements() `value' fail  name: %s path: %s", name, path);
        goto out2;
    }

    sz = (*env)->GetArrayLength(env, jbvalue);

    if (setxattr(path, name, value, (size_t) sz, 0, options) < 0) {
        throw_ioexc(env, "setxattr(2) fail  errno: %d options: %#x sz: %d name: %s path: %s", errno, options, sz, name, path);
    }

    (*env)->ReleaseByteArrayElements(env, jbvalue, value, JNI_ABORT);
out2:
    free(name);
out1:
    free(path);
}

JNIEXPORT void JNICALL
Java_net_trineo_xattr4j_XAttr4J__1fsetxattr(
        JNIEnv *env,
        jclass cls,
        jint fd,
        jbyteArray bname,
        jbyteArray bvalue,
        jint options)
{
    char *name;
    jbyte *value;
    jsize sz;

    name = get_cstr_bytes(env, bname);
    if (name == NULL) {
        throw_ioexc(env, "get_cstr_bytes() `name' fail  errno: %d", errno);
        return;
    }

    value = (*env)->GetByteArrayElements(env, bvalue, NULL);
    if (value == NULL) {
        throw_ioexc(env, "JNIEnv->GetByteArrayElements() `value' fail  fd: %d name: %s", fd, name);
        goto out;
    }

    sz = (*env)->GetArrayLength(env, bvalue);
    if (fsetxattr(fd, name, value, (size_t) sz, 0, options) < 0) {
        throw_ioexc(env, "fsetxattr(2) fail  errno: %d fd: %d name: %s sz: %d options: %#x", errno, fd, name, sz, options);
    }

    (*env)->ReleaseByteArrayElements(env, bvalue, value, JNI_ABORT);
out:
    free(name);
}

JNIEXPORT void JNICALL
Java_net_trineo_xattr4j_XAttr4J__1removexattr(
        JNIEnv *env,
        jclass cls,
        jbyteArray jbpath,
        jbyteArray jbname,
        jint options,
        jboolean force)
{
    int ok;
    char *path;
    char *name;

    path = get_cstr_bytes(env, jbpath);
    if (path == NULL) {
        throw_ioexc(env, "get_cstr_bytes() path fail  errno: %d", errno);
        return;
    }

    name = get_cstr_bytes(env, jbname);
    if (name == NULL) {
        throw_ioexc(env, "get_cstr_bytes() name fail  errno: %d", errno);
        goto out;
    }

    ok = !removexattr(path, name, options);
    if (!ok && (!force || (errno != ENOENT && errno != ENOATTR))) {
        throw_ioexc(env, "removexattr(2) fail  force: %d errno: %d options: %#x name: %s path: %s", force, errno, options, name, path);
    }

    free(name);
out:
    free(path);
}

JNIEXPORT void JNICALL
Java_net_trineo_xattr4j_XAttr4J__1fremovexattr(
        JNIEnv *env,
        jclass cls,
        jint fd,
        jbyteArray bname,
        jint options,
        jboolean force)
{
    int e;
    char *name;

    name = get_cstr_bytes(env, bname);
    if (name == NULL) {
        throw_ioexc(env, "get_cstr_bytes() `name' fail  errno: %d", errno);
        return;
    }

    e = fremovexattr(fd, name, options);
    if (e != 0 && (!force || (errno != ENOENT && errno != ENOATTR))) {
        throw_ioexc(env, "fremovexattr(2) fail  errno: %d fd: %d name: %s options: %#x force: %d", errno, fd, name, options, force);
    }

    free(name);
}

/**
 * List extended attribute names for a given path
 * @jbpath      UTF-8 encoded path
 * @options     Options passed to listxattr(2)
 * @return      An java.lang.String array contains all extended attribute names
 *              An empty array if given path have no xattr at all
 * @throws      IOException if operation cannot complete or failed
 */
JNIEXPORT jobjectArray JNICALL
Java_net_trineo_xattr4j_XAttr4J__1listxattr(
        JNIEnv *env,
        jclass cls,
        jbyteArray jbpath,
        jint options)
{
    jobjectArray arr = NULL;
    char *path;
    ssize_t sz;
    ssize_t sz2;
    char *namebuf;
    char *cursor;
    size_t cnt, i;
    jstring *jnamebuf;

    path = get_cstr_bytes(env, jbpath);
    if (path == NULL) {
        throw_ioexc(env, "get_cstr_bytes() path fail  errno: %d", errno);
        goto out1;
    }

out_replay:
    sz = listxattr(path, NULL, 0, options);
    if (sz < 0) {
        throw_ioexc(env, "listxattr(2) fail  errno: %d options: %#x path: %s", errno, options, path);
        goto out2;
    }

    if (sz == 0) {
        arr = (*env)->NewObjectArray(env, 0, java_lang_String, NULL);
        if (arr == NULL) throw_ioexc(env, "JNIEnv->NewObjectArray() fail  sz: %zd", sz);
        goto out2;
    }

    namebuf = (char *) malloc(sz);
    if (namebuf == NULL) {
        throw_ioexc(env, "malloc(3) fail  errno: %d options: %#x sz: %zd path: %s", errno, options, sz, path);
        goto out2;
    }

    sz2 = listxattr(path, namebuf, sz, options);
    if (sz2 < 0) {
        if (errno == ERANGE) {
            free(namebuf);
            LOG("TOCTTOU BUG in listxattr(2)  errno: %d options: %#x sz: %zd path: %s", errno, options, sz, path);
            goto out_replay;
        }

        throw_ioexc(env, "listxattr(2) fail  errno: %d options: %#x sz: %zd path: %s", errno, options, sz, path);
        goto out3;
    }

    sz = sz2;       /* NOTE: 0 <= sz2 <= sz */
    if (sz == 0) {
        arr = (*env)->NewObjectArray(env, 0, java_lang_String, NULL);
        if (arr == NULL) throw_ioexc(env, "JNIEnv->NewObjectArray() fail  sz: %zd", sz);
        goto out3;
    }

    cnt = 0;
    cursor = namebuf;
    do {
        cnt++;
        cursor += strlen(cursor) + 1; /* +1 for trailing EOS(end-of-string) */
    } while (cursor - namebuf < sz);

    jnamebuf = (jstring *) malloc(sizeof(jstring *) * cnt);
    if (jnamebuf == NULL) {
        throw_ioexc(env, "malloc(3) fail size: %zu  errno: %d options: %#x sz: %zd path: %s",
                            sizeof(jstring *) * cnt, errno, options, sz, path);
        goto out3;
    }

    cursor = namebuf;
    for (i = 0; i < cnt; i++) {
        jnamebuf[i] = (*env)->NewStringUTF(env, cursor);
        if (jnamebuf[i] == NULL) {
            throw_ioexc(env, "JNIEnv->NewStringUTF() fail  i: %zu cursor: %s options: %#x sz: %zd path: %s",
                                i, cursor, options, sz, path);
            while (i--) (*env)->DeleteLocalRef(env, jnamebuf[i]);
            goto out4;
        }
        cursor += strlen(cursor) + 1;
    }

    arr = (*env)->NewObjectArray(env, cnt, java_lang_String, NULL);
    if (arr == NULL) {
        /* i must be cnt at this moment */
        while (i--) (*env)->DeleteLocalRef(env, jnamebuf[i]);
        throw_ioexc(env, "JNIEnv->NewObjectArray() fail  cnt: %zu options: %#x sz: %zd path: %s",
                            cnt, options, sz, path);
        goto out4;
    }

    for (i = 0; i < cnt; i++) {
        (*env)->SetObjectArrayElement(env, arr, i, jnamebuf[i]);
        /* see: https://stackoverflow.com/q/4369974/10725426 */
        (*env)->DeleteLocalRef(env, jnamebuf[i]);
    }

out4:
    free(jnamebuf);
out3:
    free(namebuf);
out2:
    free(path);
out1:
    return arr;
}

/**
 * see: Java_net_trineo_xattr4j_XAttr4J__1listxattr()
 */
JNIEXPORT jobjectArray JNICALL
Java_net_trineo_xattr4j_XAttr4J__1flistxattr(
        JNIEnv *env,
        jclass cls,
        jint fd,
        jint options)
{
    jobjectArray arr = NULL;
    ssize_t sz;
    ssize_t sz2;
    char *namebuf;
    char *cursor;
    size_t cnt, i;
    jstring *jnamebuf;

out_replay:
    sz = flistxattr(fd, NULL, 0, options);
    if (sz < 0) {
        throw_ioexc(env, "flistxattr(2) fail  errno: %d fd: %d options: %#x", errno, fd, options);
        goto out1;
    }

    if (sz == 0) {
        arr = (*env)->NewObjectArray(env, 0, java_lang_String, NULL);
        if (arr == NULL) throw_ioexc(env, "JNIEnv->NewObjectArray() fail  sz: %zd", sz);
        goto out1;
    }

    namebuf = (char *) malloc(sz);
    if (namebuf == NULL) {
        throw_ioexc(env, "malloc(3) fail  errno: %d fd: %d options: %#x sz: %zd", errno, fd, options, sz);
        goto out1;
    }

    sz2 = flistxattr(fd, namebuf, sz, options);
    if (sz2 < 0) {
        if (errno == ERANGE) {
            free(namebuf);
            LOG("TOCTTOU BUG in flistxattr(2)  errno: %d fd: %d options: %#x sz: %zd", errno, fd, options, sz);
            goto out_replay;
        }

        throw_ioexc(env, "flistxattr(2) fail  errno: %d fd: %d options: %#x sz: %zd", errno, fd, options, sz);
        goto out2;
    }

    sz = sz2;       /* NOTE: 0 <= sz2 <= sz */
    if (sz == 0) {
        arr = (*env)->NewObjectArray(env, 0, java_lang_String, NULL);
        if (arr == NULL) throw_ioexc(env, "JNIEnv->NewObjectArray() fail  sz: %zd", sz);
        goto out2;
    }

    cnt = 0;
    cursor = namebuf;
    do {
        cnt++;
        cursor += strlen(cursor) + 1; /* +1 for trailing EOS(end-of-string) */
    } while (cursor - namebuf < sz);

    jnamebuf = (jstring *) malloc(sizeof(jstring *) * cnt);
    if (jnamebuf == NULL) {
        throw_ioexc(env, "malloc(3) fail size: %zu  errno: %d fd: %d options: %#x sz: %zd",
                            sizeof(jstring *) * cnt, errno, fd, options, sz);
        goto out2;
    }

    cursor = namebuf;
    for (i = 0; i < cnt; i++) {
        jnamebuf[i] = (*env)->NewStringUTF(env, cursor);
        if (jnamebuf[i] == NULL) {
            throw_ioexc(env, "JNIEnv->NewStringUTF() fail  i: %zu cursor: %s fd: %d options: %#x sz: %zd",
                                i, cursor, fd, options, sz);
            while (i--) (*env)->DeleteLocalRef(env, jnamebuf[i]);
            goto out3;
        }
        cursor += strlen(cursor) + 1;
    }

    arr = (*env)->NewObjectArray(env, cnt, java_lang_String, NULL);
    if (arr == NULL) {
        /* i must be cnt at this moment */
        while (i--) (*env)->DeleteLocalRef(env, jnamebuf[i]);
        throw_ioexc(env, "JNIEnv->NewObjectArray() fail  cnt: %zu fd: %d options: %#x sz: %zd",
                            cnt, fd, options, sz);
        goto out3;
    }

    for (i = 0; i < cnt; i++) {
        (*env)->SetObjectArrayElement(env, arr, i, jnamebuf[i]);
        /* see: https://stackoverflow.com/q/4369974/10725426 */
        (*env)->DeleteLocalRef(env, jnamebuf[i]);
    }

out3:
    free(jnamebuf);
out2:
    free(namebuf);
out1:
    return arr;
}

/**
 * Fast wrapper of getxattr(2)
 * @return      size of the xattr value     possibly zero
 * @throws      IOException if I/O error or denoted xattr name do not exist
 *
 * This fast wrapper can be used to probe existence of a specific xattr
 *
 * ASK: use a jboolean to differentiate IOExceptions between
 *      denoted xattr do not exist and other IO errors
 */
JNIEXPORT jlong JNICALL
Java_net_trineo_xattr4j_XAttr4J__1sizexattr(
        JNIEnv *env,
        jclass cls,
        jbyteArray jbpath,
        jbyteArray jbname,
        jint options)
{
    jlong sz = -1;  /* -1 isn't a valid size */
    char *path;
    char *name;

    path = get_cstr_bytes(env, jbpath);
    if (path == NULL) {
        throw_ioexc(env, "get_cstr_bytes() path fail  errno: %d", errno);
        goto out1;
    }

    name = get_cstr_bytes(env, jbname);
    if (name == NULL) {
        throw_ioexc(env, "get_cstr_bytes() name fail  errno: %d", errno);
        goto out2;
    }

    sz = (jlong) getxattr(path, name, NULL, 0, 0, options);
    if (sz < 0) {
        throw_ioexc(env, "getxattr(2) fail  errno: %d options: %#x name: %s path: %s", errno, options, name, path);
    }

    free(name);
out2:
    free(path);
out1:
    return sz;
}

JNIEXPORT jlong JNICALL
Java_net_trineo_xattr4j_XAttr4J__1fsizexattr(
        JNIEnv *env,
        jclass cls,
        jint fd,
        jbyteArray bname,
        jint options)
{
    jlong sz = -1;  /* -1 isn't a valid size */
    char *name;

    name = get_cstr_bytes(env, bname);
    if (name == NULL) {
        throw_ioexc(env, "get_cstr_bytes() `name' fail  errno: %d", errno);
        goto out;
    }

    sz = (jlong) fgetxattr(fd, name, NULL, 0, 0, options);
    if (sz < 0) {
        throw_ioexc(env, "fgetxattr(2) fail  errno: %d fd: %d name: %s options: %#x", errno, fd, name, options);
    }

    free(name);
out:
    return sz;
}

JNIEXPORT jboolean JNICALL
Java_net_trineo_xattr4j_XAttr4J__1existxattr(
        JNIEnv *env,
        jclass cls,
        jbyteArray jbpath,
        jbyteArray jbname,
        jint options)
{
    jboolean exists = JNI_FALSE;
    char *path;
    char *name;

    path = get_cstr_bytes(env, jbpath);
    if (path == NULL) {
        throw_ioexc(env, "get_cstr_bytes() path fail  errno: %d", errno);
        goto out1;
    }

    name = get_cstr_bytes(env, jbname);
    if (name == NULL) {
        throw_ioexc(env, "get_cstr_bytes() name fail  errno: %d", errno);
        goto out2;
    }

    exists = getxattr(path, name, NULL, 0, 0, options) >= 0;
    if (!exists && errno != ENOATTR) {
        /* Throw if given `path' doesn't exist(ENOENT) */
        throw_ioexc(env, "getxattr(2) fail  errno: %d options: %#x name: %s path: %s", errno, options, name, path);
    }

    free(name);
out2:
    free(path);
out1:
    return exists;
}

JNIEXPORT jboolean JNICALL
Java_net_trineo_xattr4j_XAttr4J__1fexistxattr(
        JNIEnv *env,
        jclass cls,
        jint fd,
        jbyteArray bname,
        jint options)
{
    jboolean exists = JNI_FALSE;
    char *name;

    name = get_cstr_bytes(env, bname);
    if (name == NULL) {
        throw_ioexc(env, "get_cstr_bytes() `name' fail  errno: %d", errno);
        goto out;
    }

    exists = fgetxattr(fd, name, NULL, 0, 0, options) >= 0;
    if (!exists && errno != ENOATTR) {
        /* Will throw if given `fd' isn't valid(EBADF) */
        throw_ioexc(env, "fgetxattr(2) fail  errno: %d fd:%d name: %s options: %#x", errno, fd, name, options);
    }

    free(name);
out:
    return exists;
}

JNIEXPORT jlong JNICALL
Java_net_trineo_xattr4j_XAttr4J__1xattrsizebits(
        JNIEnv *env,
        jclass cls,
        jbyteArray bpath)
{
    long bits = -1;
    char *path;

    path = get_cstr_bytes(env, bpath);
    if (path == NULL) {
        throw_ioexc(env, "get_cstr_bytes() `path' fail  errno: %d", errno);
        goto out;
    }

    errno = 0;
    bits = pathconf(path, _PC_XATTR_SIZE_BITS);
    /* see: pathconf(2) RETURN VALUES */
    if (bits < 0 && errno != 0) {
        throw_ioexc(env, "pathconf(2) fail  errno: %d path: %s", errno, path);
    }

    free(path);
out:
    return bits;    /* Possibly be -1? */
}

JNIEXPORT jlong JNICALL
Java_net_trineo_xattr4j_XAttr4J__1fxattrsizebits(
        JNIEnv *env,
        jclass cls,
        jint fd)
{
    long bits = -1;

    errno = 0;
    bits = fpathconf(fd, _PC_XATTR_SIZE_BITS);
    /* see: fpathconf(2) RETURN VALUES */
    if (bits < 0 && errno != 0) {
        throw_ioexc(env, "fpathconf(2) fail  errno: %d fd: %d", errno, fd);
    }

    return bits;    /* Possibly be -1? */
}

