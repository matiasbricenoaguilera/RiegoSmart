(function() {
  const path = window.location.pathname.split("/").pop() || "index.html";
  const isLoginPage = path === "login.html";
  const ok = localStorage.getItem("rs_auth") === "1";

  if (!ok && !isLoginPage) {
    window.location.href = "login.html";
    return;
  }
})();
