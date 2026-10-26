import forge.asio.blocking;
import forge.asio.affine;
import forge.asio.compute;
import forge.asio.gate;
import forge.asio.notification;
import forge.asio.runtime;
import forge.asio.task;

int main() {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto scheduler = forge::asio::task::scheduler{runtime};
   auto compute = forge::asio::compute::pool{forge::asio::compute::pool::options{.worker_threads = 1}};
   auto gate = forge::asio::gate{};
   auto notification = forge::asio::notification{};
   auto lane = forge::asio::affine::lane{};
   auto scheduled = scheduler.submit(forge::asio::task::task{.name = "package-completion", .work = [] {}});
   forge::asio::blocking::run(runtime, scheduled.wait());
   const auto completed = scheduled.completed();

   auto ticket = forge::asio::blocking::run(runtime, gate.acquire());
   ticket.release();
   notification.notify();

   const auto result = forge::asio::blocking::run(
       runtime, compute.get_executor().execute({.name = "package-smoke"}, [] { return 42; }));
   const auto affine_result = forge::asio::blocking::run(
       runtime, lane.get_executor().execute({.name = "affine-package-smoke"}, [] { return 43; }));
   forge::asio::blocking::run(runtime, lane.shutdown());
   forge::asio::blocking::run(runtime, compute.shutdown());
   forge::asio::blocking::run(runtime, scheduler.shutdown());
   return result == 42 && affine_result == 43 && completed ? 0 : 1;
}
