/*

    strscan.c

    Copyright (c) 1999-2003 Minero Aoki <aamine@loveruby.net>

    This program is free software.
    You can distribute/modify this program under the terms of
    the Ruby License. For details, see the file COPYING.

    $Id$

*/


#include "ruby.h"
#include "re.h"
#include "version.h"

#if (RUBY_VERSION_CODE < 150)
#  define rb_eRangeError rb_eArgError
#  define rb_obj_freeze(obj) rb_str_freeze(obj)
#endif

#define STRSCAN_VERSION "0.7.0"

/* =======================================================================
                         Data Type Definitions
   ======================================================================= */

static VALUE StringScanner;
static VALUE ScanError;

struct strscanner
{
    /* multi-purpose flags */
    unsigned long flags;
#define FLAG_MATCHED (1 << 0)

    /* the string to scan */
    VALUE str;
    
    /* scan pointers */
    long prev;   /* legal only when MATCHED_P(s) */
    long curr;   /* always legal */

    /* the regexp register; legal only when MATCHED_P(s) */
    struct re_registers regs;
};

#define MATCHED_P(s)          ((s)->flags & FLAG_MATCHED)
#define MATCHED(s)             (s)->flags |= FLAG_MATCHED
#define CLEAR_MATCH_STATUS(s)  (s)->flags &= ~FLAG_MATCHED

#define S_PBEG(s)  (RSTRING((s)->str)->ptr)
#define S_LEN(s)  (RSTRING((s)->str)->len)
#define S_PEND(s)  (S_PBEG(s) + S_LEN(s))
#define CURPTR(s) (S_PBEG(s) + (s)->curr)
#define S_RESTLEN(s) (S_LEN(s) - (s)->curr)

#define EOS_P(s) ((s)->curr >= RSTRING(p->str)->len)

#define GET_SCANNER(obj,var) do {\
    Data_Get_Struct(obj, struct strscanner, var);\
    if (NIL_P(var->str)) rb_raise(rb_eArgError, "uninitialized StringScanner object");\
} while (0)

/* =======================================================================
                            Function Prototypes
   ======================================================================= */

static VALUE infect _((VALUE str, struct strscanner *p));
static VALUE extract_range _((struct strscanner *p, long beg_i, long end_i));
static VALUE extract_beg_len _((struct strscanner *p, long beg_i, long len));

static void strscan_mark _((struct strscanner *p));
static void strscan_free _((struct strscanner *p));
static VALUE strscan_s_allocate _((VALUE klass));
static VALUE strscan_initialize _((int argc, VALUE *argv, VALUE self));

static VALUE strscan_s_mustc _((VALUE self));
static VALUE strscan_terminate _((VALUE self));
static VALUE strscan_get_string _((VALUE self));
static VALUE strscan_set_string _((VALUE self, VALUE str));
static VALUE strscan_concat _((VALUE self, VALUE str));
static VALUE strscan_get_pos _((VALUE self));
static VALUE strscan_set_pos _((VALUE self, VALUE pos));
static VALUE strscan_do_scan _((VALUE self, VALUE regex,
                                int succptr, int getstr, int headonly));
static VALUE strscan_scan _((VALUE self, VALUE re));
static VALUE strscan_match_p _((VALUE self, VALUE re));
static VALUE strscan_skip _((VALUE self, VALUE re));
static VALUE strscan_check _((VALUE self, VALUE re));
static VALUE strscan_scan_full _((VALUE self, VALUE re,
                                  VALUE succp, VALUE getp));
static VALUE strscan_scan_until _((VALUE self, VALUE re));
static VALUE strscan_skip_until _((VALUE self, VALUE re));
static VALUE strscan_check_until _((VALUE self, VALUE re));
static VALUE strscan_search_full _((VALUE self, VALUE re,
                                    VALUE succp, VALUE getp));
static void adjust_registers_to_matched _((struct strscanner *p));
static VALUE strscan_getch _((VALUE self));
static VALUE strscan_get_byte _((VALUE self));
static VALUE strscan_peek _((VALUE self, VALUE len));
static VALUE strscan_unscan _((VALUE self));
static VALUE strscan_bol_p _((VALUE self));
static VALUE strscan_eos_p _((VALUE self));
static VALUE strscan_rest_p _((VALUE self));
static VALUE strscan_matched_p _((VALUE self));
static VALUE strscan_matched _((VALUE self));
static VALUE strscan_matched_size _((VALUE self));
static VALUE strscan_aref _((VALUE self, VALUE idx));
static VALUE strscan_pre_match _((VALUE self));
static VALUE strscan_post_match _((VALUE self));
static VALUE strscan_rest _((VALUE self));
static VALUE strscan_rest_size _((VALUE self));

static VALUE strscan_inspect _((VALUE self));
static VALUE inspect1 _((struct strscanner *p));
static VALUE inspect2 _((struct strscanner *p));

/* =======================================================================
                                   Utils
   ======================================================================= */

static VALUE
infect(str, p)
    VALUE str;
    struct strscanner *p;
{
    OBJ_INFECT(str, p->str);
    return str;
}

static VALUE
extract_range(p, beg_i, end_i)
    struct strscanner *p;
    long beg_i, end_i;
{
    if (beg_i > S_LEN(p)) return Qnil;
    if (end_i > S_LEN(p))
        end_i = S_LEN(p);
    return infect(rb_str_new(S_PBEG(p) + beg_i, end_i - beg_i), p);
}

static VALUE
extract_beg_len(p, beg_i, len)
    struct strscanner *p;
    long beg_i, len;
{
    if (beg_i > S_LEN(p)) return Qnil;
    if (beg_i + len > S_LEN(p))
        len = S_LEN(p) - beg_i;
    return infect(rb_str_new(S_PBEG(p) + beg_i, len), p);
}


/* =======================================================================
                               Constructor
   ======================================================================= */


static void
strscan_mark(p)
    struct strscanner *p;
{
    rb_gc_mark(p->str);
}

static void
strscan_free(p)
    struct strscanner *p;
{
    re_free_registers(&(p->regs));
    memset(p, sizeof(struct strscanner), 0);
    free(p);
}

static VALUE
strscan_s_allocate(klass)
    VALUE klass;
{
    struct strscanner *p;
    
    p = ALLOC(struct strscanner);
    MEMZERO(p, struct strscanner, 1);
    CLEAR_MATCH_STATUS(p);
    MEMZERO(&(p->regs), struct re_registers, 1);
    p->str = Qnil;
    return Data_Wrap_Struct(klass, strscan_mark, strscan_free, p);
}

/*
 * call-seq: StringScanner.new(string, dup)
 *
 * Creates a new StringScanner object to scan over the given +string+.  If
 * +dup+ is +true+, a copy of the string is used instead.  Either way, the string
 * is frozen before scanning commences.
 */
static VALUE
strscan_initialize(argc, argv, self)
    int argc;
    VALUE *argv;
    VALUE self;
{
    struct strscanner *p;
    VALUE str, need_dup;

    Data_Get_Struct(self, struct strscanner, p);
    rb_scan_args(argc, argv, "11", &str, &need_dup);
    StringValue(str);
    p->str = str;

    return self;
}


/* =======================================================================
                          Instance Methods
   ======================================================================= */

/*
 * FIXME: prevent this method from being documented.
 */
static VALUE
strscan_s_mustc(self)
    VALUE self;
{
    return self;
}

/*
 * Reset the scan pointer (index 0) and clear matching data.
 */
static VALUE
strscan_reset(self)
    VALUE self;
{
    struct strscanner *p;

    GET_SCANNER(self, p);
    p->curr = 0;
    CLEAR_MATCH_STATUS(p);
    return self;
}

/*
 * call-seq:
 *   terminate
 *   clear
 *
 * Set the scan pointer to the end of the string and clear matching data.
 */
static VALUE
strscan_terminate(self)
    VALUE self;
{
    struct strscanner *p;

    GET_SCANNER(self, p);
    p->curr = S_LEN(p);
    CLEAR_MATCH_STATUS(p);
    return self;
}

/*
 * Returns the string being scanned.
 */
static VALUE
strscan_get_string(self)
    VALUE self;
{
    struct strscanner *p;

    GET_SCANNER(self, p);
    return p->str;
}

/*
 * call-seq: string=(str)
 *
 * Changes the string being scanned to +str+ and resets the scanner.  Returns
 * +str+.
 */
static VALUE
strscan_set_string(self, str)
    VALUE self, str;
{
    struct strscanner *p;

    Data_Get_Struct(self, struct strscanner, p);
    StringValue(str);
    p->str = rb_str_dup(str);
    rb_obj_freeze(p->str);
    p->curr = 0;
    CLEAR_MATCH_STATUS(p);
    return str;
}

/*
 * call-seq:
 *   concat(str)
 *   << (str)
 *
 * Appends +str+ to the string being scanned.
 *
 *   s = StringScanner.new("Fri Dec 12 1975 14:39")
 *   s << " +1000 GMT"
 *   s.string             # -> "Fri Dec 12 1975 14:39 +1000 GMT"
 */
static VALUE
strscan_concat(self, str)
    VALUE self, str;
{
    struct strscanner *p;

    GET_SCANNER(self, p);
    StringValue(str);
    rb_str_append(p->str, str);
    return self;
}

/*
 * Returns the position of the scan pointer.  In the 'reset' position, this
 * value is zero.  In the 'terminated' position (i.e. the string is exhausted),
 * this value is the length of the string.
 *
 * In short, it's a 1-based index into the string.
 *
 *   s = StringScanner.new('test string')
 *   s.pos               # -> 0
 *   s.scan_until /str/  # -> "test str"
 *   s.pos               # -> 8
 *   s.terminate         # -> #<StringScanner fin>
 *   s.pos               # -> 11
 */
static VALUE
strscan_get_pos(self)
    VALUE self;
{
    struct strscanner *p;

    GET_SCANNER(self, p);
    return INT2FIX(p->curr);
}

/*
 * call-seq: pos=(n)
 *
 * Modify the scan pointer.
 *
 *   s = StringScanner.new('test string')
 *   s.pos = 7            # -> 7
 *   s.rest               # -> "ring"
 */
static VALUE
strscan_set_pos(self, v)
    VALUE self, v;
{
    struct strscanner *p;
    long i;

    GET_SCANNER(self, p);
    i = NUM2INT(v);
    if (i < 0) i += S_LEN(p);
    if (i < 0) rb_raise(rb_eRangeError, "index out of range");
    if (i > S_LEN(p)) rb_raise(rb_eRangeError, "index out of range");
    p->curr = i;
    return INT2NUM(i);
}


/* I should implement this function? */
#define strscan_prepare_re(re)  /* none */

static VALUE
strscan_do_scan(self, regex, succptr, getstr, headonly)
    VALUE self, regex;
    int succptr, getstr, headonly;
{
    struct strscanner *p;
    int ret;

    Check_Type(regex, T_REGEXP);
    GET_SCANNER(self, p);

    CLEAR_MATCH_STATUS(p);
    if (EOS_P(p)) {
        return Qnil;
    }
    strscan_prepare_re(regex);
    if (headonly) {
        ret = re_match(RREGEXP(regex)->ptr,
                       CURPTR(p), S_RESTLEN(p),
                       0,
                       &(p->regs));
    }
    else {
        ret = re_search(RREGEXP(regex)->ptr,
                        CURPTR(p), S_RESTLEN(p),
                        0,
                        S_RESTLEN(p),
                        &(p->regs));
    }

    if (ret == -2) rb_raise(ScanError, "regexp buffer overflow");
    if (ret < 0) {
        /* not matched */
        return Qnil;
    }

    MATCHED(p);
    p->prev = p->curr;
    if (succptr) {
        p->curr += p->regs.end[0];
    }
    if (getstr) {
        return extract_beg_len(p, p->prev, p->regs.end[0]);
    }
    else {
        return INT2FIX(p->regs.end[0]);
    }
}

/*
 * call-seq:
 *   scanner.scan(pattern) => String
 *
 */

/*
 * call-seq:
 *   scan(pattern)
 *
 * Tries to match with +pattern+ at the current position. If there's a match,
 * the scanner advances the "scan pointer" and returns the matched string.
 * Otherwise, the scanner returns +nil+.
 *
 *   s = StringScanner.new('test string')
 *   p s.scan(/\w+/)   # -> "test"
 *   p s.scan(/\w+/)   # -> nil
 *   p s.scan(/\s+/)   # -> " "
 *   p s.scan(/\w+/)   # -> "string"
 *   p s.scan(/./)     # -> nil
 *
 */
static VALUE
strscan_scan(self, re)
    VALUE self, re;
{
    return strscan_do_scan(self, re, 1, 1, 1);
}

/*
 * call-seq: match?(pattern)
 *
 * Tests whether the given +pattern+ is matched from the current scan pointer.
 * Returns the length of the match, or +nil+.  The scan pointer is not advanced.
 *
 *   s = StringScanner.new('test string')
 *   p s.match?(/\w+/)   # -> 4
 *   p s.match?(/\w+/)   # -> 4
 *   p s.match?(/\s+/)   # -> nil
 */
static VALUE
strscan_match_p(self, re)
    VALUE self, re;
{
    return strscan_do_scan(self, re, 0, 0, 1);
}

/*
 * call-seq: skip(pattern)
 *
 * Attempts to skip over the given +pattern+ beginning with the scan pointer.
 * If it matches, the scan pointer is advanced to the end of the match, and the
 * length of the match is returned.  Otherwise, +nil+ is returned.
 *
 * It's similar to #scan, but without returning the matched string.
 *
 *   s = StringScanner.new('test string')
 *   p s.skip(/\w+/)   # -> 4
 *   p s.skip(/\w+/)   # -> nil
 *   p s.skip(/\s+/)   # -> 1
 *   p s.skip(/\w+/)   # -> 6
 *   p s.skip(/./)     # -> nil
 *
 */
static VALUE
strscan_skip(self, re)
    VALUE self, re;
{
    return strscan_do_scan(self, re, 1, 0, 1);
}

/*
 * call-seq: check(pattern)
 *
 * This returns the value that #scan would return, without advancing the scan
 * pointer.  The match register is affected, though.
 *
 *   s = StringScanner.new("Fri Dec 12 1975 14:39")
 *   s.check /Fri/               # -> "Fri"
 *   s.pos                       # -> 0
 *   s.matched                   # -> "Fri"
 *   s.check /12/                # -> nil
 *   s.matched                   # -> nil
 *
 * Mnemonic: it "checks" to see whether a #scan will return a value.
 */
static VALUE
strscan_check(self, re)
    VALUE self, re;
{
    return strscan_do_scan(self, re, 0, 1, 1);
}

/*
 * DOCUMENTATION
 */
static VALUE
strscan_scan_full(self, re, s, f)
    VALUE self, re, s, f;
{
    return strscan_do_scan(self, re, RTEST(s), RTEST(f), 1);
}


/*
 * call-seq: scan_until(pattern)
 *
 * Scans the string _until_ the +pattern+ is matched.  Returns the substring up
 * to and including the end of the match, advancing the scan pointer to that
 * location. If there is no match, +nil+ is returned.
 *
 *   s = StringScanner.new("Fri Dec 12 1975 14:39")
 *   s.scan_until(/1/)        # -> "Fri Dec 1"
 *   s.pre_match              # -> "Fri Dec "
 *   s.scan_until(/XYZ/)      # -> nil
 */
static VALUE
strscan_scan_until(self, re)
    VALUE self, re;
{
    return strscan_do_scan(self, re, 1, 1, 0);
}

/*
 * call-seq: exist?(pattern)
 *
 * Looks _ahead_ to see if the +pattern+ exists _anywhere_ in the string,
 * without advancing the scan pointer.  This predicates whether a #scan_until
 * will return a value.
 *
 *   s = StringScanner.new('test string')
 *   s.exist? /s/            # -> 3
 *   s.scan /test/           # -> "test"
 *   s.exist? /s/            # -> 6
 *   s.exist? /e/            # -> nil
 */
static VALUE
strscan_exist_p(self, re)
    VALUE self, re;
{
    return strscan_do_scan(self, re, 0, 0, 0);
}

/*
 * call-seq: skip_until(pattern)
 *
 * Advances the scan pointer until +pattern+ is matched and consumed.  Returns
 * the number of characters advanced, or +nil+ if no match was found.
 *
 * Look ahead to match +pattern+, and advance the scan pointer to the _end_ of the
 * match.  Return the number of characters advanced, or +nil+ if the match was
 * unsuccessful.
 *
 * It's similar to #scan_until, but without returning the intervening string.
 *
 *   s = StringScanner.new("Fri Dec 12 1975 14:39")
 *   s.skip_until /12/           # -> 10
 *   s                           # 
 */
static VALUE
strscan_skip_until(self, re)
    VALUE self, re;
{
    return strscan_do_scan(self, re, 1, 0, 0);
}

/*
 * call-seq: check_until(pattern)
 *
 * This returns the value that #scan_until would return, without advancing the
 * scan pointer.  The match register is affected, though.
 *
 *   s = StringScanner.new("Fri Dec 12 1975 14:39")
 *   s.check_until /12/          # -> "Fri Dec 12"
 *   s.pos                       # -> 0
 *   s.matched                   # -> 12
 *
 * Mnemonic: it "checks" to see whether a #scan_until will return a value.
 */
static VALUE
strscan_check_until(self, re)
    VALUE self, re;
{
    return strscan_do_scan(self, re, 0, 1, 0);
}

/*
 * DOCUMENTATION
 */
static VALUE
strscan_search_full(self, re, s, f)
    VALUE self, re, s, f;
{
    return strscan_do_scan(self, re, RTEST(s), RTEST(f), 0);
}

/* DANGEROUS; need to synchronize with regex.c */
static void
adjust_registers_to_matched(p)
    struct strscanner *p;
{
    if (p->regs.allocated == 0) {
        p->regs.beg = ALLOC_N(int, RE_NREGS);
        p->regs.end = ALLOC_N(int, RE_NREGS);
        p->regs.allocated = RE_NREGS;
    }
    p->regs.num_regs = 1;
    p->regs.beg[0] = 0;
    p->regs.end[0] = p->curr - p->prev;
}

/*
 * Scans one character and returns it.
 *
 *   s = StringScanner.new('ab')
 *   s.getch           # => 'a'
 *   s.getch           # => 'b'
 *   s.getch           # => nil
 */
static VALUE
strscan_getch(self)
    VALUE self;
{
    struct strscanner *p;
    long len;

    GET_SCANNER(self, p);
    CLEAR_MATCH_STATUS(p);
    if (EOS_P(p))
        return Qnil;

    len = mbclen(*CURPTR(p));
    if (p->curr + len > S_LEN(p))
        len = S_LEN(p) - p->curr;
    p->prev = p->curr;
    p->curr += len;
    MATCHED(p);
    adjust_registers_to_matched(p);
    return extract_range(p, p->prev + p->regs.beg[0],
                            p->prev + p->regs.end[0]);
}

/*
 * Scans one byte and returns it.  Similar to, but not the same as, #getch.
 */
static VALUE
strscan_get_byte(self)
    VALUE self;
{
    struct strscanner *p;

    GET_SCANNER(self, p);
    CLEAR_MATCH_STATUS(p);
    if (EOS_P(p))
        return Qnil;

    p->prev = p->curr;
    p->curr++;
    MATCHED(p);
    adjust_registers_to_matched(p);
    return extract_range(p, p->prev + p->regs.beg[0],
                            p->prev + p->regs.end[0]);
}


/*
 * call-seq: peek(len)
 *
 * Extracts a string corresponding to <tt>string[pos,len]</tt>, without
 * advancing the scan pointer.
 *
 *   s = StringScanner.new('test string')
 *   s.peek(7)                # -> "test st"
 *   s.peek(7)                # -> "test st"
 *
 */
static VALUE
strscan_peek(self, vlen)
    VALUE self, vlen;
{
    struct strscanner *p;
    long len;

    GET_SCANNER(self, p);

    len = NUM2LONG(vlen);
    if (EOS_P(p))
        return infect(rb_str_new("", 0), p);

    if (p->curr + len > S_LEN(p))
        len = S_LEN(p) - p->curr;
    return extract_beg_len(p, p->curr, len);
}

/*
 * Set the scan pointer to the previous position.  Only one previous position is
 * remembered, and it changes with each scanning operation.
 *
 *   s = StringScanner.new('test string')
 *   s.scan(/\w+/)        # -> "test"
 *   s.unscan
 *   s.scan(/../)         # -> "te"
 *   s.scan(/\d/)         # -> nil
 *   s.unscan             # ScanError: cannot unscan: prev match had failed
 */
static VALUE
strscan_unscan(self)
    VALUE self;
{
    struct strscanner *p;

    GET_SCANNER(self, p);
    if (! MATCHED_P(p))
        rb_raise(ScanError, "cannot unscan: prev match had failed");

    p->curr = p->prev;
    CLEAR_MATCH_STATUS(p);
    return self;
}

/*
 * Returns +true+ iff the scan pointer is at the beginning of the string.
 *
 *   s = StringScanner.new('test string')
 *   s.bol?                 # These two
 *   s.pos == 0             # are equivalent.
 */
static VALUE
strscan_bol_p(self)
    VALUE self;
{
    struct strscanner *p;

    GET_SCANNER(self, p);
    if (CURPTR(p) > S_PEND(p)) return Qnil;
    if (p->curr == 0) return Qtrue;
    return (*(CURPTR(p) - 1) == '\n') ? Qtrue : Qfalse;
}

/*
 * Returns +true+ if the scan pointer is at the end of the string.
 */
static VALUE
strscan_eos_p(self)
    VALUE self;
{
    struct strscanner *p;

    GET_SCANNER(self, p);
    if (EOS_P(p))
        return Qtrue;
    else
        return Qfalse;
}

/*
 * Returns true iff there is more data in the string.  See #eos?.
 *
 *   s = StringScanner.new('test string')
 *   s.eos?              # These two
 *   s.rest?             # are opposites.
 */
static VALUE
strscan_rest_p(self)
    VALUE self;
{
    struct strscanner *p;

    GET_SCANNER(self, p);
    if (EOS_P(p))
        return Qfalse;
    else
        return Qtrue;
}


/*
 * Returns +true+ iff the last match was successful.
 *
 *   s = StringScanner.new('test string')
 *   s.match?(/\w+/)     # -> 4
 *   s.matched?          # -> true
 *   s.match?(/\d+/)     # -> nil
 *   s.matched?          # -> false
 */
static VALUE
strscan_matched_p(self)
    VALUE self;
{
    struct strscanner *p;

    GET_SCANNER(self, p);
    if (MATCHED_P(p))
        return Qtrue;
    else
        return Qfalse;
}

/*
 * Returns the last matched string.
 * 
 *   s = StringScanner.new('test string')
 *   s.match?(/\w+/)     # -> 4
 *   s.matched           # -> "test"
 */
static VALUE
strscan_matched(self)
    VALUE self;
{
    struct strscanner *p;

    GET_SCANNER(self, p);
    if (! MATCHED_P(p)) return Qnil;

    return extract_range(p, p->prev + p->regs.beg[0],
                            p->prev + p->regs.end[0]);
}

/*
 * Returns the size of the most recent match (see #matched), or +nil+ if there
 * was no recent match.
 *
 *   s = StringScanner.new('test string')
 *   s.check /\w+/           # -> "test"
 *   s.matched_size          # -> 4
 *   s.check /\d+/           # -> nil
 *   s.matched_size          # -> nil
 */
static VALUE
strscan_matched_size(self)
    VALUE self;
{
    struct strscanner *p;

    GET_SCANNER(self, p);
    if (! MATCHED_P(p)) return Qnil;

    return INT2NUM(p->regs.end[0] - p->regs.beg[0]);
}

/*
 * call-seq: [](n)
 *
 * Return the n-th subgroup in the most recent match.
 *
 *   s = StringScanner.new("Fri Dec 12 1975 14:39")
 *   s.scan(/(\w+) (\w+) (\d+) /)       # -> "Fri Dec 12 "
 *   s[0]                               # -> "Fri Dec 12 "
 *   s[1]                               # -> "Fri"
 *   s[2]                               # -> "Dec"
 *   s[3]                               # -> "12"
 *   s.post_match                       # -> "1975 14:39"
 *   s.pre_match                        # -> ""
 */
static VALUE
strscan_aref(self, idx)
    VALUE self, idx;
{
    struct strscanner *p;
    long i;

    GET_SCANNER(self, p);
    if (! MATCHED_P(p))        return Qnil;
    
    i = NUM2LONG(idx);
    if (i < 0)
        i += p->regs.num_regs;
    if (i < 0)                 return Qnil;
    if (i >= p->regs.num_regs) return Qnil;
    if (p->regs.beg[i] == -1)  return Qnil;

    return extract_range(p, p->prev + p->regs.beg[i],
                            p->prev + p->regs.end[i]);
}

/*
 * Return the <i><b>pre</b>-match</i> (in the regular expression sense) of the last scan.
 *
 *   s = StringScanner.new('test string')
 *   s.scan(/\w+/)           # -> "test"
 *   s.scan(/\s+/)           # -> " "
 *   s.pre_match             # -> "test"
 *   s.post_match            # -> "string"
 */
static VALUE
strscan_pre_match(self)
    VALUE self;
{
    struct strscanner *p;

    GET_SCANNER(self, p);
    if (! MATCHED_P(p)) return Qnil;

    return extract_range(p, 0, p->prev + p->regs.beg[0]);
}

/*
 * Return the <i><b>post</b>-match</i> (in the regular expression sense) of the last scan.
 *
 *   s = StringScanner.new('test string')
 *   s.scan(/\w+/)           # -> "test"
 *   s.scan(/\s+/)           # -> " "
 *   s.pre_match             # -> "test"
 *   s.post_match            # -> "string"
 */
static VALUE
strscan_post_match(self)
    VALUE self;
{
    struct strscanner *p;

    GET_SCANNER(self, p);
    if (! MATCHED_P(p)) return Qnil;

    return extract_range(p, p->prev + p->regs.end[0], S_LEN(p));
}


/*
 * Returns the "rest" of the string (i.e. everything after the scan pointer).
 * If there is no more data, it returns <tt>""</tt>.
 */
static VALUE
strscan_rest(self)
    VALUE self;
{
    struct strscanner *p;

    GET_SCANNER(self, p);
    if (EOS_P(p)) {
        return infect(rb_str_new("", 0), p);
    }
    return extract_range(p, p->curr, S_LEN(p));
}

/*
 * <tt>s.rest_size</tt> is equivalent to <tt>s.rest.size</tt>.
 */
static VALUE
strscan_rest_size(self)
    VALUE self;
{
    struct strscanner *p;
    long i;

    GET_SCANNER(self, p);
    if (EOS_P(p)) {
        return INT2FIX(0);
    }

    i = S_LEN(p) - p->curr;
    return INT2FIX(i);
}


#define INSPECT_LENGTH 5
#define BUFSIZE 256

/*
 * Returns a string that represents the StringScanner object, showing:
 * - the current position
 * - the size of the string
 * - the characters surrounding the scan pointer
 *
 *   s = StringScanner.new("Fri Dec 12 1975 14:39")
 *   s.inspect            # -> '#<StringScanner 0/21 @ "Fri D...">'
 *   s.scan_until /12/    # -> "Fri Dec 12"
 *   s.inspect            # -> '#<StringScanner 10/21 "...ec 12" @ " 1975...">'
 */
static VALUE
strscan_inspect(self)
    VALUE self;
{
    struct strscanner *p;
    char buf[BUFSIZE];
    long len;
    VALUE result;
    VALUE a, b;

    Data_Get_Struct(self, struct strscanner, p);
    if (NIL_P(p->str)) {
        len = snprintf(buf, BUFSIZE, "#<%s (uninitialized)>",
                       rb_class2name(CLASS_OF(self)));
        return infect(rb_str_new(buf, len), p);
    }
    if (EOS_P(p)) {
        len = snprintf(buf, BUFSIZE, "#<%s fin>",
                       rb_class2name(CLASS_OF(self)));
        return infect(rb_str_new(buf, len), p);
    }
    if (p->curr == 0) {
        b = inspect2(p);
        len = snprintf(buf, BUFSIZE, "#<%s %ld/%ld @ %s>",
                       rb_class2name(CLASS_OF(self)),
                       p->curr, S_LEN(p),
                       RSTRING(b)->ptr);
        return infect(rb_str_new(buf, len), p);
    }
    a = inspect1(p);
    b = inspect2(p);
    len = snprintf(buf, BUFSIZE, "#<%s %ld/%ld %s @ %s>",
                   rb_class2name(CLASS_OF(self)),
                   p->curr, S_LEN(p),
                   RSTRING(a)->ptr,
                   RSTRING(b)->ptr);
    return infect(rb_str_new(buf, len), p);
}

static VALUE
inspect1(p)
    struct strscanner *p;
{
    char buf[BUFSIZE];
    char *bp = buf;
    long len;

    if (p->curr == 0) return rb_str_new2("");
    if (p->curr > INSPECT_LENGTH) {
        strcpy(bp, "..."); bp += 3;
        len = INSPECT_LENGTH;
    }
    else {
        len = p->curr;
    }
    memcpy(bp, CURPTR(p) - len, len); bp += len;
    return rb_str_dump(rb_str_new(buf, bp - buf));
}

static VALUE
inspect2(p)
    struct strscanner *p;
{
    char buf[BUFSIZE];
    char *bp = buf;
    long len;

    if (EOS_P(p)) return rb_str_new2("");
    len = S_LEN(p) - p->curr;
    if (len > INSPECT_LENGTH) {
        len = INSPECT_LENGTH;
        memcpy(bp, CURPTR(p), len); bp += len;
        strcpy(bp, "..."); bp += 3;
    }
    else {
        memcpy(bp, CURPTR(p), len); bp += len;
    }
    return rb_str_dump(rb_str_new(buf, bp - buf));
}

/* =======================================================================
                              Ruby Interface
   ======================================================================= */

/*
 * StringScanner provides for lexical scanning operations on a String.  Here is
 * an example of its usage:
 *
 *   s = StringScanner.new('This is an example string')
 *   s.eos?               # -> false
 *   
 *   p s.scan(/\w+/)      # -> "This"
 *   p s.scan(/\w+/)      # -> nil
 *   p s.scan(/\s+/)      # -> " "
 *   p s.scan(/\s+/)      # -> nil
 *   p s.scan(/\w+/)      # -> "is"
 *   s.eos?               # -> false
 *   
 *   p s.scan(/\s+/)      # -> " "
 *   p s.scan(/\w+/)      # -> "an"
 *   p s.scan(/\s+/)      # -> " "
 *   p s.scan(/\w+/)      # -> "example"
 *   p s.scan(/\s+/)      # -> " "
 *   p s.scan(/\w+/)      # -> "string"
 *   s.eos?               # -> true
 *   
 *   p s.scan(/\s+/)      # -> nil
 *   p s.scan(/\w+/)      # -> nil
 *
 * Scanning a string means remembering the position of a <i>scan pointer</i>,
 * which is just an index.  The scan pointer effectively points _between_
 * characters.  (XXX: get this right - is it between or not?)
 *
 * Given the string "test string", here are the pertinent scan pointer
 * positions:
 *
 *     t e s t   s t r i n g
 *   0 1 2 ...             1
 *                         0
 *
 * When you #scan for a pattern (a regular expression), the match must occur
 * at the character after the scan pointer.  If you use #scan_until, then the
 * match can occur anywhere after the scan pointer.  In both cases, the scan
 * pointer moves <i>just beyond</i> the last character of the match, ready to
 * scan again from the next character onwards.  This is demonstrated by the
 * example above.
 *
 * == Method Categories
 *
 * There are other methods besides the plain scanners.  You can look ahead in
 * the string without actually scanning.  You can access the most recent match.
 * You can modify the string being scanned, reset or terminate the scanner,
 * find out or change the position of the scan pointer, skip ahead, and so on.
 * 
 * === Advancing the Scan Pointer
 *
 * getch
 * getbyte
 * scan
 * scan_until
 * skip
 * skip_until
 *
 * === Looking Ahead
 *
 * check
 * check_until
 * exist?
 * match?
 * peek
 *
 * === Finding Where we Are
 *
 * bol?
 * eos?
 * rest?
 * rest_size
 *
 * === Setting Where we Are
 *
 * reset
 * terminate
 * pos=
 * 
 * === Match Data
 *
 * matched
 * matched?
 * matched_size
 * pre_match
 * post_match
 *
 * === Miscellaneous
 *
 * <<
 * string=
 * string
 * unscan
 *
 * === Unknown
 *
 * scan_full
 * search_full
 *
 * There are aliases to several of the methods.
 */
void
Init_strscan()
{
    ID id_scanerr = rb_intern("ScanError");
    volatile VALUE tmp;

    if (rb_const_defined(rb_cObject, id_scanerr)) {
        ScanError = rb_const_get(rb_cObject, id_scanerr);
    }
    else {
        ScanError = rb_define_class_id(id_scanerr, rb_eStandardError);
    }

    StringScanner = rb_define_class("StringScanner", rb_cObject);
    tmp = rb_str_new2(STRSCAN_VERSION);
    rb_obj_freeze(tmp);
    rb_const_set(StringScanner, rb_intern("Version"), tmp);
    tmp = rb_str_new2("$Id$");
    rb_obj_freeze(tmp);
    rb_const_set(StringScanner, rb_intern("Id"), tmp);
    
    rb_define_alloc_func(StringScanner, strscan_s_allocate);
    rb_define_private_method(StringScanner, "initialize", strscan_initialize, -1);
    rb_define_singleton_method(StringScanner, "must_C_version", strscan_s_mustc, 0);
    rb_define_method(StringScanner, "reset",       strscan_reset,       0);
    rb_define_method(StringScanner, "terminate",   strscan_terminate,   0);
    rb_define_method(StringScanner, "clear",       strscan_terminate,   0);
    rb_define_method(StringScanner, "string",      strscan_get_string,  0);
    rb_define_method(StringScanner, "string=",     strscan_set_string,  1);
    rb_define_method(StringScanner, "concat",      strscan_concat,      1);
    rb_define_method(StringScanner, "<<",          strscan_concat,      1);
    rb_define_method(StringScanner, "pos",         strscan_get_pos,     0);
    rb_define_method(StringScanner, "pos=",        strscan_set_pos,     1);
    rb_define_method(StringScanner, "pointer",     strscan_get_pos,     0);
    rb_define_method(StringScanner, "pointer=",    strscan_set_pos,     1);

    rb_define_method(StringScanner, "scan",        strscan_scan,        1);
    rb_define_method(StringScanner, "skip",        strscan_skip,        1);
    rb_define_method(StringScanner, "match?",      strscan_match_p,     1);
    rb_define_method(StringScanner, "check",       strscan_check,       1);
    rb_define_method(StringScanner, "scan_full",   strscan_scan_full,   3);

    rb_define_method(StringScanner, "scan_until",  strscan_scan_until,  1);
    rb_define_method(StringScanner, "skip_until",  strscan_skip_until,  1);
    rb_define_method(StringScanner, "exist?",      strscan_exist_p,     1);
    rb_define_method(StringScanner, "check_until", strscan_check_until, 1);
    rb_define_method(StringScanner, "search_full", strscan_search_full, 3);

    rb_define_method(StringScanner, "getch",       strscan_getch,       0);
    rb_define_method(StringScanner, "get_byte",    strscan_get_byte,    0);
    rb_define_method(StringScanner, "getbyte",     strscan_get_byte,    0);
    rb_define_method(StringScanner, "peek",        strscan_peek,        1);
    rb_define_method(StringScanner, "peep",        strscan_peek,        1);

    rb_define_method(StringScanner, "unscan",      strscan_unscan,      0);

    rb_define_method(StringScanner, "beginning_of_line?", strscan_bol_p, 0);
    rb_define_method(StringScanner, "bol?",        strscan_bol_p,       0);
    rb_define_method(StringScanner, "eos?",        strscan_eos_p,       0);
    rb_define_method(StringScanner, "empty?",      strscan_eos_p,       0);
    rb_define_method(StringScanner, "rest?",       strscan_rest_p,      0);

    rb_define_method(StringScanner, "matched?",    strscan_matched_p,   0);
    rb_define_method(StringScanner, "matched",     strscan_matched,     0);
    rb_define_method(StringScanner, "matched_size", strscan_matched_size, 0);
    rb_define_method(StringScanner, "matchedsize", strscan_matched_size, 0);
    rb_define_method(StringScanner, "[]",          strscan_aref,        1);
    rb_define_method(StringScanner, "pre_match",   strscan_pre_match,   0);
    rb_define_method(StringScanner, "post_match",  strscan_post_match,  0);

    rb_define_method(StringScanner, "rest",        strscan_rest,        0);
    rb_define_method(StringScanner, "rest_size",   strscan_rest_size,   0);
    rb_define_method(StringScanner, "restsize",    strscan_rest_size,   0);

    rb_define_method(StringScanner, "inspect",     strscan_inspect,     0);
}
