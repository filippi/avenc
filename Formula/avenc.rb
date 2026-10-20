# First release: HEAD only. Generate a stable formula with packaging/homebrew.py.
class Avenc < Formula
  desc "Video compression and reconstruction of meteorological GRIB grids"
  homepage "https://github.com/filippi/avenc"
  head "https://github.com/filippi/avenc.git", branch: "main"
  license "Apache-2.0"

  depends_on "cmake" => :build
  depends_on "eccodes"
  depends_on "ffmpeg"

  def install
    system "cmake", "-S", ".", "-B", "build", *std_cmake_args,
           "-DENABLE_FAST_MATH=OFF", "-DENABLE_NATIVE_ARCH=OFF", "-DBUILD_TESTING=ON"
    system "cmake", "--build", "build"
    system "ctest", "--test-dir", "build", "--output-on-failure"
    system "cmake", "--install", "build"
  end

  test do
    assert_match version.to_s, shell_output("#{bin}/grib2avenc --version") unless build.head?
    system bin/"avenc_test_generator", "--ni", "512", "--nj", "128", "--nt", "2", "--nlev", "1", "--out-dir", "fixture"
    system bin/"grib2avenc", "fixture/synthetic_all.grib", "test.mp4"
    system bin/"avenc2grib", "test.mp4", "roundtrip.grib"
    assert_predicate testpath/"roundtrip.grib", :exist?
  end
end
