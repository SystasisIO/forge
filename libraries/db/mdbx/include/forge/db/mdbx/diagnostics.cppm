export module forge.db.mdbx.diagnostics;

export namespace forge::db::mdbx {

class diagnostics final {
 public:
   // Claims libmdbx's process-global debug callback for the process lifetime.
   // Call before opening environments; driver::open also claims it once.
   static void claim_process();
   [[nodiscard]] static bool installed() noexcept;
   // Caller must hold neither vendor nor DB-owner mutexes.
   static void drain() noexcept;
};

} // namespace forge::db::mdbx
