require 'mkmf'

dir_config("dbm")

if dblib = with_config("dbm-type", nil)
  dblib = dblib.split(/[ ,]+/)
else
  dblib = %w(libc db db2 db1 db5 db4 db3 gdbm gdbm_compat qdbm)
end

headers = {
  "libc" => ["ndbm.h"], # 4.3BSD original ndbm, Berkeley DB 1 in 4.4BSD libc.
  "db" => ["db.h"],
  "db1" => ["db1/ndbm.h", "db1.h", "ndbm.h"],
  "db2" => ["db2/db.h", "db2.h", "db.h"],
  "db3" => ["db3/db.h", "db3.h", "db.h"],
  "db4" => ["db4/db.h", "db4.h", "db.h"],
  "db5" => ["db5/db.h", "db5.h", "db.h"],
  "gdbm" => ["gdbm-ndbm.h", "ndbm.h", "gdbm/ndbm.h"], # gdbm until 1.8.0
  "gdbm_compat" => ["gdbm-ndbm.h", "ndbm.h", "gdbm/ndbm.h"], # gdbm since 1.8.1
  "qdbm" => ["relic.h", "qdbm/relic.h"],
}

class << headers
  attr_accessor :found
  attr_accessor :defs
end
headers.found = []
headers.defs = nil

def headers.db_check(db, hdr)
  old_libs = $libs.dup
  old_defs = $defs.dup
  result = db_check2(db, hdr)
  if !result
    $libs = old_libs
    $defs = old_defs
  end
  result
end

def have_libvar(var, headers = nil, opt = "", &b)
  checking_for checking_message([*var].compact.join(' '), headers, opt) do
    try_libvar(var, headers, opt, &b)
  end
end

def try_libvar(var, headers = nil, opt = "", &b)
  var, type = *var
  if try_link(<<"SRC", opt, &b)
#{cpp_include(headers)}
/*top*/
int main(int argc, char *argv[]) {
  typedef #{type || 'int'} conftest_type;
  extern conftest_type #{var};
  conftest_type *conftest_var = &#{var};
  return 0;
}
SRC
    $defs.push(format("-DHAVE_LIBVAR_%s", var.tr_cpp))
    true
  else
    false
  end
end


def headers.db_check2(db, hdr)
  hsearch = nil

  case db
  when /^db[2-5]?$/
    hsearch = "-DDB_DBM_HSEARCH"
  when "gdbm_compat"
    have_library("gdbm") or return false
  end

  if !have_type("DBM", hdr, hsearch)
    return false
  end

  # 'libc' means ndbm is provided by libc.
  # 4.3BSD original ndbm is contained in libc.
  # 4.4BSD (and its derivatives such as NetBSD) contains Berkeley DB 1 in libc.
  if !(db == 'libc' ? have_func('dbm_open("", 0, 0)', hdr, hsearch) :
                      have_library(db, 'dbm_open("", 0, 0)', hdr, hsearch))
    return false
  end

  # Skip a mismatch of Berkeley DB's ndbm.h and old gdbm library.
  #
  # dbm_clearerr() should be available for any ndbm implementation.
  # It is available since the original (4.3BSD) ndbm and standardized by POSIX.
  #
  # However "can't resolve symbol 'dbm_clearerr'" problem may be caused by
  # header/library mismatch: Berkeley DB ndbm.h and gdbm library until 1.8.3.
  # gdbm (until 1.8.3) provides dbm_clearerr() as a empty macro in the header
  # and the library don't provide dbm_clearerr().
  # Berkeley DB provides dbm_clearerr() as a usual function.
  # So Berkeley DB header with gdbm library causes the problem.
  #
  if !have_func('dbm_clearerr((DBM *)0)', hdr, hsearch)
    return false
  end

  # ndbm.h is provided by the original (4.3BSD) dbm,
  # Berkeley DB 1 in libc of 4.4BSD and
  # ndbm compatibility layer of gdbm.
  # So, try to check header/library mismatch.
  #
  # Assumption: There are no insane environment which libc provides ndbm
  # functions but ndbm.h is not for that.
  # 
  if hdr == 'ndbm.h' && db != 'libc'
    # Berkeley DB's ndbm.h (since 1.85 at least) includes db.h and
    # it defines _DB_H_.
    if /\Adb\d?\z/ !~ db && have_macro('_DB_H_', hdr, hsearch)
      return false
    end

    # Recent GDBM's ndbm.h, since 1.9, includes gdbm.h and it defines _GDBM_H_.
    # ndbm compatibility layer of GDBM is provided by libgdbm (until 1.8.0)
    # and libgdbm_compat (since 1.8.1).
    if /\Agdbm/ !~ db && have_macro('_GDBM_H_', hdr, hsearch)
      return false
    end
    
    # 4.3BSD's ndbm.h defines _DBM_IOERR.
    # The original ndbm is provided by libc in 4.3BSD.
    if have_macro('_DBM_IOERR', hdr, hsearch)
      return false
    end
  end

  case db
  when /\Adb\d?\z/
    have_func('db_version((int *)0, (int *)0, (int *)0)', hdr, hsearch)
  when /\Agdbm/
    have_var("gdbm_version", hdr, hsearch)
    # gdbm_version is not declared by ndbm.h until gdbm 1.8.3.
    # We can't include ndbm.h and gdbm.h because they both define datum type.
    # ndbm.h includes gdbm.h and gdbm_version is declared since gdbm 1.9.
    have_libvar(["gdbm_version", "char *"], hdr, hsearch)
  when /\Aqdbm\z/
    have_var("dpversion", hdr, hsearch)
  end
  if hsearch
    $defs << hsearch
    @defs = hsearch
  end
  $defs << '-DDBM_HDR="<'+hdr+'>"'
  @found << hdr

  true
end

if dblib.any? {|db| headers.fetch(db, ["ndbm.h"]).any? {|hdr| headers.db_check(db, hdr) } }
  have_header("cdefs.h")
  have_header("sys/cdefs.h")
  have_func("dbm_pagfno((DBM *)0)", headers.found, headers.defs)
  have_func("dbm_dirfno((DBM *)0)", headers.found, headers.defs)
  convertible_int("datum.dsize", headers.found, headers.defs)
  create_makefile("dbm")
end
