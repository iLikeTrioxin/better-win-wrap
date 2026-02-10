{
  lib,
  hyprland,
  hyprlandPlugins,
}:
hyprlandPlugins.mkHyprlandPlugin {
  pluginName = "better-win-wrap";
  version = "0.1";
  src = ./.;

  inherit (hyprland) nativeBuildInputs;

  meta = with lib; {
    homepage = "https://github.com/iLikeTrioxin/better-win-wrap";
    description = "private version of hyprwinwrap";
    license = licenses.bsd3;
    platforms = platforms.linux;
  };
}
