# The executive: dxexec and the libraries it is linked from. Source lists are
# the expanded *_SOURCES of the autotools Makefile.am files in each folder.
#
# The libraries call each other in a cycle (libdx, dpexec, dxmods), so each is
# an object library and dxexec links the objects directly.

find_package(X11 REQUIRED)
find_package(OpenGL REQUIRED COMPONENTS OpenGL GLX)
find_package(Threads REQUIRED)

# glibc checks writes into buffers of known size at run time and aborts with
# a message instead of corrupting memory. Needs optimisation, so not in Debug.
set(dx_fortify $<$<AND:$<C_COMPILER_ID:GNU,Clang>,$<NOT:$<CONFIG:Debug>>>:_FORTIFY_SOURCE=2>)

set(gen ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${gen}/libdx ${gen}/dxmods ${gen}/dpexec)

# --- Generated sources --------------------------------------------------------

# libdx class files: each .X gives a header and an implementation.
set(dx_classes
    array camera clipped field group light object private screen string
    xform interp fieldinterp groupinterp fle2D linesII1D linesRR1D linesRI1D
    trisRI2D quadsRR2D cubesII cubesRR tetras quadsII2D)
file(GLOB dx_class_inputs ${CMAKE_CURRENT_SOURCE_DIR}/libdx/*.X)
set(dx_class_outputs)
foreach(cls IN LISTS dx_classes)
  set(x ${CMAKE_CURRENT_SOURCE_DIR}/libdx/${cls}Class.X)
  foreach(kind h c)
    set(out ${gen}/libdx/${cls}Class.${kind})
    add_custom_command(
      OUTPUT ${out}
      COMMAND Python3::Interpreter ${DX_CODEGEN}/dx_class.py -${kind} ${x} -o ${out}
      DEPENDS ${dx_class_inputs} ${DX_CODEGEN}/dx_class.py
      COMMENT "Generating ${cls}Class.${kind}"
      VERBATIM)
    list(APPEND dx_class_outputs ${out})
  endforeach()
endforeach()

# libdx macro templates (*.Z), expanded by the C preprocessor.
set(dx_cpp "${CMAKE_C_COMPILER} -E")
set(dx_template_outputs)
foreach(name buffer lbshade plane triangle quad)
  set(out ${gen}/libdx/${name}.c)
  add_custom_command(
    OUTPUT ${out}
    COMMAND Python3::Interpreter ${DX_CODEGEN}/zexpand.py ${name}
            --srcdir ${CMAKE_CURRENT_SOURCE_DIR}/libdx --cpp ${dx_cpp}
            --work ${gen}/libdx/work-${name} -o ${out}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/libdx/${name}.Z ${DX_CODEGEN}/zexpand.py
    COMMENT "Expanding ${name}.Z"
    VERBATIM)
  list(APPEND dx_template_outputs ${out})
endforeach()

# Module definitions: dxmdf.src -> dx.mdf -> user.c (the module table).
set(mdf_src ${CMAKE_CURRENT_SOURCE_DIR}/dxmods/dxmdf.src)
set(mdf_flags -I${DX_GENERATED_INCLUDE} -I${CMAKE_SOURCE_DIR}/include
              -I${CMAKE_CURRENT_SOURCE_DIR}/dpexec -D${DX_ARCH} -DHAVE_CONFIG_H)
add_custom_command(
  OUTPUT ${gen}/dxmods/dx.mdf
  COMMAND Python3::Interpreter ${DX_CODEGEN}/mdf_preprocess.py --mode exec
          --cpp ${dx_cpp} ${mdf_flags} ${mdf_src} -o ${gen}/dxmods/dx.mdf
  DEPENDS ${mdf_src} ${DX_CODEGEN}/mdf_preprocess.py
          ${DX_GENERATED_INCLUDE}/dxconfig.h
  COMMENT "Preprocessing dx.mdf"
  VERBATIM)
add_custom_command(
  OUTPUT ${gen}/dxmods/user.c
  COMMAND Python3::Interpreter ${DX_CODEGEN}/mdf2c.py ${gen}/dxmods/dx.mdf
          -o ${gen}/dxmods/user.c
  DEPENDS ${gen}/dxmods/dx.mdf ${DX_CODEGEN}/mdf2c.py
  COMMENT "Generating user.c"
  VERBATIM)

# Build stamp read by dpexec/command.c.
string(TIMESTAMP dx_build_date "%a %b %d %H:%M:%S UTC %Y" UTC)
cmake_host_system_information(RESULT dx_host QUERY HOSTNAME OS_NAME OS_RELEASE)
string(REPLACE ";" " " dx_host "${dx_host}")
file(WRITE ${gen}/dpexec/compile.h
     "#define EX_COM_DATE \"${dx_build_date}\"\n#define EX_COM_HOST \"${dx_host}\"\n")

add_custom_target(dx_generated
  DEPENDS ${dx_class_outputs} ${dx_template_outputs} ${gen}/dxmods/user.c)

# --- Libraries ----------------------------------------------------------------

function(dx_object_library name dir)
  list(TRANSFORM ARGN PREPEND ${CMAKE_CURRENT_SOURCE_DIR}/${dir}/)
  add_library(${name} OBJECT ${ARGN})
  target_include_directories(${name} PRIVATE
    ${DX_GENERATED_INCLUDE} ${CMAKE_SOURCE_DIR}/include)
  target_compile_definitions(${name} PRIVATE
    HAVE_CONFIG_H ${DX_ARCH} _GNU_SOURCE ${dx_fortify})
  add_dependencies(${name} dx_generated)
endfunction()

dx_object_library(dx_libdx libdx
    array.c basic.c box.c lbcamera.c clipped.c
    composite.c cstarray.c field.c lbgrid.c group.c helper.c init.c lblight.c
    lock.c matrix.c mesharray.c lbmessage.c multigrid.c neighbors.c object.c
    patharray.c lbprivate.c productarray.c sharedarray.c regulararray.c
    lbscreen.c series.c string.c stringtable.c timing.c lbvector.c version.c
    xform.c userinit.c axes.c client.c lbcolor.c component.c
    displayfb.c displayx.c displayutil.c displayw.c windows.c edfdata.c
    edfdict.c edfio.c edflex.c edfobj.c edfparse.c edfprint.c lbextract.c
    fileio.c grow.c hash.c image.c invalid.c irreggrow.c lbmap.c gather.c
    line.c lbpartition.c partreg.c parts.c permute.c paint.c plock.c lbprint.c
    reggrow.c rwobject.c seglist.c stats.c lbtext.c lbtransform.c binSort.c
    interp.c fieldinterp.c groupinterp.c fle2D.c linesII1D.c linesRR1D.c
    linesRI1D.c trisRI2D.c quadsRR2D.c cubesII.c cubesRR.c tetras.c quadsII2D.c
    point.c tile.c volume.c xfield.c
    xwindow.c zclipT.c zclipQ.c notify.c lbpick.c lbmsgs.c
    mem.c memory.c)
target_sources(dx_libdx PRIVATE ${dx_class_outputs} ${dx_template_outputs})
target_include_directories(dx_libdx PRIVATE
    ${gen}/libdx ${CMAKE_CURRENT_SOURCE_DIR}/libdx)
# The autotools rules compile these two with an extra -O (so -O1 overall).
set_source_files_properties(libdx/zclipT.c libdx/zclipQ.c
    PROPERTIES COMPILE_OPTIONS -O)

dx_object_library(dx_dpexec dpexec
    dpattribute.c background.c cache.c cachegraph.c ccm.c command.c context.c
    crc.c d.c distconnect.c distpacket.c distqueue.c dxmain.c dxpfsmgr.c
    evalgraph.c exobject.c function.c graph.c graph2.c graphqueue.c help.c
    instrument.c lex.c license.c loader.c log.c macro.c nodereadb.c
    nodewriteb.c packet.c dpparse.c parsemdf.c path.c pendingcmds.c queue.c
    remote.c rih.c rq.c socket.c status.c swap.c task.c userinter.c utils.c
    variable.c vcr.c optarg.c yuiif.c sfile.c ddx.c)
target_include_directories(dx_dpexec PRIVATE
    ${gen}/dpexec ${CMAKE_CURRENT_SOURCE_DIR}/libdx)

dx_object_library(dx_dxmods dxmods
    ambientlight.c append.c arrange.c attribute.c autocolor.c autoaxes.c
    autoglyph.c autogray.c autoregrid.c band.c bspline.c cachescene.c camera.c
    caption.c categorize.c catstats.c changetype.c changemember.c clipbox.c
    clipplane.c collect.c collectmulti.c collectnamed.c collectser.c color.c
    colorbar.c colormap.c compute.c compute2.c connect.c construct.c convert.c
    copy.c describe.c direction.c display.c divcurl.c dxlmessage.c
    dxloutvalue.c dxuiscene.c echo.c enumerate.c equalize.c export.c extract.c
    facenormals.c filter.c format.c fourier.c getscene.c glyph.c gradient.c
    grid.c histogram.c imagemessage.c import.c import_ss.c include.c inquire.c
    isolate.c isosurface.c integer.c integerlist.c keyin.c light.c list.c
    lookup.c macroutil.c map.c maptoplane.c mark.c measure.c message.c
    normals.c options.c overlay.c parse.c partition.c pick.c pickinv.c pie.c
    plot.c post.c print.c programctl.c quantize.c rdimagewin.c readimage.c
    reduce.c refine.c regrid.c remove.c rename.c render.c reorient.c replace.c
    ribbon.c rotate.c route.c rubbersheet.c sample.c scalar.c scalarlist.c
    scale.c scalescreen.c screen.c select.c selector.c shade.c showboundary.c
    showbox.c showconnect.c showposition.c slab.c slice.c socketconnect.c
    sort.c stack.c statistics.c streakline.c streamline.c stringlegend.c
    superstate.c superwin.c switch.c system.c text.c toggle.c trace.c
    transform.c translate.c transpose.c tube.c unmark.c usage.c vector.c
    vectorlist.c verify.c visualobject.c writeimage.c simplesurf.c
    _autocolor.c _autogray.c _cat_util.c _color.c _colormap.c _complex.c
    _compoper.c _compoper1.c _compoper2.c _compcmplx.c _compinput.c _compexec.c
    _compparse.c _compputils.c _connectgrids.c _connectvor.c _construct.c
    _divcurl.c _getfield.c _gif.c _glyph.c _gradient.c _grid.c _helper_jea.c
    _im_image.c _irregstream.c _isosurface.c _maptoplane.c _newtri.c _normals.c
    _partnbrs.c _plot.c _post.c _postscript.c _refine.c _refineirr.c
    _refinereg.c _refinetopo.c _regstream.c _rgb_image.c _rubbersheet.c
    _rw_image.c _sample.c _simplesurf.c _tiff.c _tube.c _unpart.c bounds.c
    definter.c exp_gai.c genimp.c genimp_io.c genimp_parse.c importtable.c
    import_cdf.c import_cm.c import_hdf.c import_ncdf.c private.c separate.c
    superwinX.c superwinW.c tracevisual.c vectors.c vrml.c eigen.c vsincos.c
    executive.c getset.c loop.c dxlinnamed.c sequencer.c)
target_sources(dx_dxmods PRIVATE ${gen}/dxmods/user.c)
target_include_directories(dx_dxmods PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/dxmods ${CMAKE_CURRENT_SOURCE_DIR}/dpexec)

dx_object_library(dx_hwrender hwrender
    hwClientMessage.c hwClipped.c hwCursorInteractor.c hwDraw.c
    hwGroupInteractor.c hwInitScreen.c hwInteractor.c hwList.c hwMatrix.c
    hwMaterials.c hwMemory.c hwNavigateInteractor.c hwObject.c hwObjectHash.c
    hwPolyline.c hwPortHandle.c hwQmesh.c hwRender.c hwRotateInteractor.c
    hwScreen.c hwSort.c hwStereo.c hwStereoSys.c hwStereoCams.c hwTexture.c
    hwTmesh.c hwWindow.c hwZoomInteractor.c hwGather.c hwPaint.c hwUpdateview.c
    hwUserInteractor.c hwView.c hwXfield.c
    opengl/hwPortUtilOGL.c opengl/hwInteractorEchoOGL.c
    opengl/hwDisplayListsOGL.c opengl/hwPortOGL.c opengl/hwLoadOGL.c)
target_include_directories(dx_hwrender PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/hwrender ${CMAKE_CURRENT_SOURCE_DIR}/libdx)

# --- dxexec -------------------------------------------------------------------

add_executable(dxexec dxexec/main.c)
target_include_directories(dxexec PRIVATE
    ${DX_GENERATED_INCLUDE} ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/dpexec)
target_compile_definitions(dxexec PRIVATE
    HAVE_CONFIG_H ${DX_ARCH} _GNU_SOURCE ${dx_fortify})
target_link_libraries(dxexec PRIVATE
    dx_dpexec dx_dxmods dx_libdx dx_hwrender
    OpenGL::GL OpenGL::GLX OpenGL::GLU X11::X11 X11::Xext
    Threads::Threads ${CMAKE_DL_LIBS} m)
# Modules loaded at run time resolve DX's symbols from the executable
# (autotools: -Wl,-export-dynamic).
set_target_properties(dxexec PROPERTIES ENABLE_EXPORTS ON)

# Same layout as the autotools install, so DXROOT=<prefix>/dx works.
install(TARGETS dxexec RUNTIME DESTINATION dx/bin_${DX_ARCH})
install(FILES ${gen}/dxmods/dx.mdf DESTINATION dx/lib)
# Read at run time: named colours, message texts, startup settings.
install(FILES
    ${CMAKE_SOURCE_DIR}/lib/colors.txt ${CMAKE_SOURCE_DIR}/lib/messages
    ${CMAKE_SOURCE_DIR}/lib/dxrc ${CMAKE_SOURCE_DIR}/lib/dxfSaveCurrentImage.net
    DESTINATION dx/lib)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/fonts/ DESTINATION dx/fonts
        FILES_MATCHING PATTERN "*.dx")
