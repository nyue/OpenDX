// The `macro Image(...)` wrapper the legacy UI writes into a .net file so the
// executive can run Image nodes without the UI: ImageNode::printMacro in
// src/uipp/dxuilib/ImageNodeUtils.C (non-Java variant, internal caching 1, the
// default). Extracted mechanically from that function; byte-identical to the
// wrapper in samples saved by DX 4.3.3 (e.g. dxsamples programs/PlotColumn.net).

#include "NetImageMacro.h"

namespace dxui {

std::string_view legacyImageMacroText()
{
    static constexpr std::string_view text = R"DXIMAGE(macro Image(
        id,
        object,
        where,
        useVector,
        to,
        from,
        width,
        resolution,
        aspect,
        up,
        viewAngle,
        perspective,
        options,
        buttonState = 1,
        buttonUpApprox = "none",
        buttonDownApprox = "none",
        buttonUpDensity = 1,
        buttonDownDensity = 1,
        renderMode = 0,
        defaultCamera,
        reset,
        backgroundColor,
        throttle,
        RECenable = 0,
        RECfile,
        RECformat,
        RECresolution,
        RECaspect,
        AAenable = 0,
        AAlabels,
        AAticks,
        AAcorners,
        AAframe,
        AAadjust,
        AAcursor,
        AAgrid,
        AAcolors,
        AAannotation,
        AAlabelscale,
        AAfont,
        interactionMode,
        title,
        AAxTickLocs,
        AAyTickLocs,
        AAzTickLocs,
        AAxTickLabels,
        AAyTickLabels,
        AAzTickLabels,
        webOptions) -> (
        object,
        camera,
        where)
{
    ImageMessage(
        id,
        backgroundColor,
        throttle,
        RECenable,
        RECfile,
        RECformat,
        RECresolution,
        RECaspect,
        AAenable,
        AAlabels,
        AAticks,
        AAcorners,
        AAframe,
        AAadjust,
        AAcursor,
        AAgrid,
        AAcolors,
        AAannotation,
        AAlabelscale,
        AAfont,
        AAxTickLocs,
        AAyTickLocs,
        AAzTickLocs,
        AAxTickLabels,
        AAyTickLabels,
        AAzTickLabels,
        interactionMode,
        title,
        renderMode,
        buttonUpApprox,
        buttonDownApprox,
        buttonUpDensity,
        buttonDownDensity) [instance: 1, cache: 1];
    autoCamera =
        AutoCamera(
            object,
            "front",
            object,
            resolution,
            aspect,
            [0,1,0],
            perspective,
            viewAngle,
            backgroundColor) [instance: 1, cache: 1];
    realCamera =
        Camera(
            to,
            from,
            width,
            resolution,
            aspect,
            up,
            perspective,
            viewAngle,
            backgroundColor) [instance: 1, cache: 1];
    coloredDefaultCamera = 
	 UpdateCamera(defaultCamera,
            background=backgroundColor) [instance: 1, cache: 1];
    nullDefaultCamera =
        Inquire(defaultCamera,
            "is null + 1") [instance: 1, cache: 1];
    resetCamera =
        Switch(
            nullDefaultCamera,
            coloredDefaultCamera,
            autoCamera) [instance: 1, cache: 1];
    resetNull = 
        Inquire(
            reset,
            "is null + 1") [instance: 2, cache: 1];
    reset =
        Switch(
            resetNull,
            reset,
            0) [instance: 2, cache: 1];
    whichCamera =
        Compute(
            "($0 != 0 || $1 == 0) ? 1 : 2",
            reset,
            useVector) [instance: 1, cache: 1];
    camera = Switch(
            whichCamera,
            resetCamera,
            realCamera) [instance: 3, cache: 1];
    AAobject =
        AutoAxes(
            object,
            camera,
            AAlabels,
            AAticks,
            AAcorners,
            AAframe,
            AAadjust,
            AAcursor,
            AAgrid,
            AAcolors,
            AAannotation,
            AAlabelscale,
            AAfont,
            AAxTickLocs,
            AAyTickLocs,
            AAzTickLocs,
            AAxTickLabels,
            AAyTickLabels,
            AAzTickLabels) [instance: 1, cache: 1];
    switchAAenable = Compute("$0+1",
	     AAenable) [instance: 2, cache: 1];
    object = Switch(
	     switchAAenable,
	     object,
	     AAobject) [instance:4, cache: 1];
    SWapproximation_options =
        Switch(
            buttonState,
            buttonUpApprox,
            buttonDownApprox) [instance: 5, cache: 1];
    SWdensity_options =
        Switch(
            buttonState,
            buttonUpDensity,
            buttonDownDensity) [instance: 6, cache: 1];
    HWapproximation_options =
        Format(
            "%s,%s",
            buttonDownApprox,
            buttonUpApprox) [instance: 1, cache: 1];
    HWdensity_options =
        Format(
            "%d,%d",
            buttonDownDensity,
            buttonUpDensity) [instance: 2, cache: 1];
    switchRenderMode = Compute(
	     "$0+1",
	     renderMode) [instance: 3, cache: 1];
    approximation_options = Switch(
	     switchRenderMode,
            SWapproximation_options,
	     HWapproximation_options) [instance: 7, cache: 1];
    density_options = Switch(
	     switchRenderMode,
            SWdensity_options,
            HWdensity_options) [instance: 8, cache: 1];
    renderModeString = Switch(
            switchRenderMode,
            "software",
            "hardware")[instance: 9, cache: 1];
    object_tag = Inquire(
            object,
            "object tag")[instance: 3, cache: 1];
    annoted_object =
        Options(
            object,
            "send boxes",
            0,
            "cache",
            1,
            "object tag",
            object_tag,
            "ddcamera",
            whichCamera,
            "rendering approximation",
            approximation_options,
            "render every",
            density_options,
            "button state",
            buttonState,
            "rendering mode",
            renderModeString) [instance: 1, cache: 1];
    RECresNull =
        Inquire(
            RECresolution,
            "is null + 1") [instance: 4, cache: 1];
    ImageResolution =
        Inquire(
            camera,
            "camera resolution") [instance: 5, cache: 1];
    RECresolution =
        Switch(
            RECresNull,
            RECresolution,
            ImageResolution) [instance: 10, cache: 1];
    RECaspectNull =
        Inquire(
            RECaspect,
            "is null + 1") [instance: 6, cache: 1];
    ImageAspect =
        Inquire(
            camera,
            "camera aspect") [instance: 7, cache: 1];
    RECaspect =
        Switch(
            RECaspectNull,
            RECaspect,
            ImageAspect) [instance: 11, cache: 1];
    switchRECenable = Compute(
          "$0 == 0 ? 1 : (($2 == $3) && ($4 == $5)) ? ($1 == 1 ? 2 : 3) : 4",
            RECenable,
            switchRenderMode,
            RECresolution,
            ImageResolution,
            RECaspect,
	     ImageAspect) [instance: 4, cache: 1];
    NoRECobject, RECNoRerenderObject, RECNoRerHW, RECRerenderObject = Route(switchRECenable, annoted_object);
    Display(
        NoRECobject,
        camera,
        where,
        throttle) [instance: 1, cache: 1];
    image =
        Render(
            RECNoRerenderObject,
            camera) [instance: 1, cache: 1];
    Display(
        image,
        NULL,
        where,
        throttle) [instance: 2, cache: 1];
    WriteImage(
        image,
        RECfile,
        RECformat) [instance: 1, cache: 1];
    rec_where = Display(
        RECNoRerHW,
        camera,
        where,
        throttle) [instance: 1, cache: 0];
    rec_image = ReadImageWindow(
        rec_where) [instance: 1, cache: 1];
    WriteImage(
        rec_image,
        RECfile,
        RECformat) [instance: 1, cache: 1];
    RECupdateCamera =
	UpdateCamera(
	    camera,
	    resolution=RECresolution,
	    aspect=RECaspect) [instance: 2, cache: 1];
    Display(
        RECRerenderObject,
        camera,
        where,
        throttle) [instance: 1, cache: 1];
    RECRerenderObject =
	ScaleScreen(
	    RECRerenderObject,
	    NULL,
	    RECresolution,
	    camera) [instance: 1, cache: 1];
    image =
        Render(
            RECRerenderObject,
            RECupdateCamera) [instance: 2, cache: 1];
    WriteImage(
        image,
        RECfile,
        RECformat) [instance: 2, cache: 1];
}
)DXIMAGE";
    return text;
}

} // namespace dxui
