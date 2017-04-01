#include <cstdlib>
#include <iostream>
#include <vector>
#include <string>
#include <sys/types.h>
#include <iomanip>
#include <mutex>
#include <thread>
#include <fstream>
#include <sstream>

#include <pcl/conversions.h>
#include <pcl/io/pcd_io.h>
#include <pcl/point_types.h>
#include <pcl/filters/statistical_outlier_removal.h>
#include <pcl/point_cloud.h>
#include <pcl/filters/passthrough.h>
#include <pcl/kdtree/kdtree_flann.h>
#include <pcl/surface/mls.h>
#include <pcl/sample_consensus/ransac.h>
#include <pcl/sample_consensus/sac_model_plane.h>
#include <pcl/filters/voxel_grid.h>
#include <pcl/common/transforms.h>

namespace fs = boost::filesystem;


std::vector<std::string> getFileList(const std::string& path);
static bool filePredicate(const std::string &s);
void processPCD(std::string path, pcl::PointCloud<pcl::PointXYZ>::Ptr src_cloud, pcl::PointCloud<pcl::PointXYZ>::Ptr stitched_cloud);
pcl::PointCloud<pcl::PointXYZ>
    rectangularThreshold(pcl::PointCloud<pcl::PointXYZ>::Ptr src_cloud, std::vector<double> thresh_range);

// TODO: Reduce the scope of these global variables
std::mutex stitch_mutex;
std::vector<std::vector<double> > translation_and_rotation;
bool transformations_file_supplied = false;


/*          Method overview         */
// 1. Rotate & translate if TrackingInfo.txt present
// 2. Threshold z-axis (0-5m)
// 3. Threshold x & y axes (0-5m)
// 4. Remove statistical outliers for each wall
// 5. Split each wall into 5 segments (1m resolution)
// 6. RANSAC plane fit to each wall segment
// 7. Stitch wall segments together
// 8. Downsample stitched point cloud

void helpMessage()
{
    std::cout << "Usage:\n"
              << "\t-f <file>" << "\t\tProcess a single file.\n"
              << "\t-d <directory>" << "\t\tProcess all of the pcd files in a directory.\n"
              << "\t-t <txt file>" << "\t\tSupply translation and rotation information. (OPTIONAL)"
              << std::endl;
}

int main (int argc, char** argv)
{
    pcl::PointCloud<pcl::PointXYZ>::Ptr src_cloud (new pcl::PointCloud<pcl::PointXYZ> ());
    pcl::PointCloud<pcl::PointXYZ>::Ptr stitched_cloud (new pcl::PointCloud<pcl::PointXYZ> ());

    /*          Handle Input        */
    // Check whether a file has been supplied
    if (argc < 3)
    {
        helpMessage();
        return -1;
    }

    std::vector<std::string> files_to_process;
    std::string directory;
    if (!std::strcmp(argv[1], "-f"))
    {
        std::string filename = argv[2];
        files_to_process.push_back(filename);
        // TODO: Write a single processed pcd file to the correct location
    }
    else if (!std::strcmp(argv[1], "-d"))
    {
        directory = argv[2];
        files_to_process = getFileList(directory);
    }
    else
    {
        std::cout << "Command \"" << argv[1] << "\" not recognised." << std::endl;
        helpMessage();
        return -1;
    }

    // In the order dx, dy, dz, rotx, roty, rotz
    std::vector<std::vector<double> > translation_and_rotation_raw;
    if (argc > 4 && !std::strcmp(argv[3], "-t"))
    {
        transformations_file_supplied = true;
        std::cout << "Using position information supplied by " << argv[4] << "." << std::endl;
        std::string line;
        std::ifstream infile(argv[4]);
        const int rows_to_skip = 2;
        const int cols_to_skip = 2;
        int row = -1;
        while (std::getline(infile, line))
        {
            // Skip header rows
            if (rows_to_skip > ++row)
            {
                continue;
            }

            translation_and_rotation_raw.push_back({});

            std::istringstream iss (line);
            double val = 0.0;
            int col = 0;
            while (iss >> val)
            {
                if (cols_to_skip > col++)
                {
                    continue;
                }
                translation_and_rotation_raw[row - rows_to_skip].push_back(val);
            }
        }
    }

    // Remove non pcd files
    files_to_process.erase(
        std::remove_if(files_to_process.begin(), files_to_process.end(), filePredicate), files_to_process.end());
    if (files_to_process.size() == 0)
    {
        std::cout << "No PCD files found." << std::endl;
        return -1;
    }

    // TODO: Check the accuracy of the calculation below

    if (transformations_file_supplied)
    {
        // Use the number of known pcd files to match translation and rotation values to each file
        int num_rows_per_file = translation_and_rotation_raw.size() / files_to_process.size();
        int num_cols = translation_and_rotation_raw[0].size();
        // Find the average translation and rotation value for each axis
        for (int row = 0; row < translation_and_rotation_raw.size(); ++row)
        {
            if (row % num_rows_per_file == 0)
            {
                translation_and_rotation.push_back({0.0, 0.0, 0.0, 0.0, 0.0, 0.0});
            }
            for (int col = 0; col < num_cols; ++col)
            {
                int new_row = row / (translation_and_rotation_raw.size() / num_rows_per_file);
                translation_and_rotation[new_row][col] += translation_and_rotation_raw[row][col];
            }
        }
        for (int row = 0; row < translation_and_rotation.size(); ++row)
        {
            for (int col = 0; col < num_cols; ++col)
            {
                translation_and_rotation[row][col] /= num_rows_per_file;
            }
        }
    }

    // Create 4 threads to process 4 files simultaneously
    // - If there are fewer than 4 files, then make that many threads
    // Once a thread returns, if there are still files to process then restart it on a new file
    std::vector<std::thread> process_threads;
    int i = 0;
    while (i < 4 && i < files_to_process.size())
    {
        // Create at most 4 threads
        std::string path = directory + files_to_process[i];
        process_threads.push_back(std::thread(processPCD, path, src_cloud, stitched_cloud));
        ++i;
        std::cout << "Processing: " << i << "/" << files_to_process.size() << std::endl;
    }
    while (i < files_to_process.size())
    {
        process_threads[0].join();
        process_threads.erase(process_threads.begin());

        // Since a thread has finished, create a new one
        std::string path = directory + files_to_process[i];
        process_threads.push_back(std::thread(processPCD, path, src_cloud, stitched_cloud));
        ++i;
        std::cout << "Processing: " << i << "/" << files_to_process.size() << std::endl;
    }
    for (auto t = process_threads.begin(); t < process_threads.end(); ++t)
    {
        t->join();
    }

    /*          Downsampling            */
    std::cout << "Downsampling stitched point cloud." << std::endl;
    pcl::PCLPointCloud2::Ptr stitched_cloud_2 (new pcl::PCLPointCloud2());
    // PointCloud -> PCLPointCloud2
    pcl::toPCLPointCloud2(*stitched_cloud, *stitched_cloud_2);
    pcl::VoxelGrid<pcl::PCLPointCloud2> vox_grid;
    vox_grid.setInputCloud(stitched_cloud_2);
    vox_grid.setLeafSize(0.1, 0.1, 0.1);
    vox_grid.filter(*stitched_cloud_2);

    std::string filename = directory + "/filtered.pcd";
    pcl::PCDWriter writer;
    writer.write(filename, *stitched_cloud_2, Eigen::Vector4f::Zero (), Eigen::Quaternionf::Identity (), false);
    return 0;
}

std::vector<std::string> getFileList(const std::string& path)
{
    std::vector<std::string> files;
    DIR *dp;
    struct dirent *dirp;
    if((dp  = opendir(path.c_str())) == NULL) {
        std::cout << "Error opening " << path << std::endl;
        return files;
    }

    while ((dirp = readdir(dp)) != NULL) {
        files.push_back(std::string(dirp->d_name));
    }
    closedir(dp);
    return files;
}

static bool filePredicate(const std::string &s)
{
    std::size_t i = s.find(".");
    return (!s.compare(".") || !s.compare("..") || !s.compare("filtered.pcd") || s.substr(i, 4).compare(".pcd"));
}

/*          Process a PCD file          */
void processPCD(std::string path, pcl::PointCloud<pcl::PointXYZ>::Ptr src_cloud, pcl::PointCloud<pcl::PointXYZ>::Ptr stitched_cloud)
{
    // Read cloud data from the supplied file
    pcl::PCDReader reader;
    reader.read(path, *src_cloud);

    /*          Rotation and translation            */
    if (transformations_file_supplied)
    {
        int idx = path.find(".");
        int i = stoi(path.substr(idx-1,1));
        Eigen::Affine3f transform = Eigen::Affine3f::Identity();
        transform.translation() << -1*translation_and_rotation[i][0], -1*translation_and_rotation[i][1],
                                    -1*translation_and_rotation[i][2];
        double rotx = translation_and_rotation[i][3],
               roty = translation_and_rotation[i][4],
               rotz = translation_and_rotation[i][5];
        transform.rotate (Eigen::AngleAxisf (-1*rotx, Eigen::Vector3f::UnitX()));
        transform.rotate (Eigen::AngleAxisf (-1*roty, Eigen::Vector3f::UnitY()));
        transform.rotate (Eigen::AngleAxisf (-1*rotz, Eigen::Vector3f::UnitZ()));
        pcl::transformPointCloud(*src_cloud, *src_cloud, transform);
    }


    pcl::PointCloud<pcl::PointXYZ>::Ptr filtered_cloud (new pcl::PointCloud<pcl::PointXYZ> ());

    /*          Passthrough filter          */
    // Depth filter
    pcl::PassThrough<pcl::PointXYZ> pass;
    pass.setInputCloud(src_cloud);
    pass.setFilterFieldName("z");
    pass.setFilterLimits(-5, 0);
    pass.filter(*filtered_cloud);

    /*          RANSAC plane fit            */
    *filtered_cloud = rectangularThreshold(filtered_cloud, {-5,0,5,-5,0,5});

    // Add the filtered points to the stitched cloud
    std::lock_guard<std::mutex> lock(stitch_mutex);
    *stitched_cloud += *filtered_cloud;
}

/*          RANSAC Plane            */
pcl::PointCloud<pcl::PointXYZ>
rectangularThreshold(pcl::PointCloud<pcl::PointXYZ>::Ptr src_cloud, std::vector<double> thresh_range)
{
    /*          I/O         */
    // returns a point cloud of 4 orthogonal planes
    // thresh_range should be in the form {min_x, mid_x, max_x, min_y, mid_y, max_y}

    const double thresh = 0.1;
    pcl::PointCloud<pcl::PointXYZ>::Ptr tmp_cloud (new pcl::PointCloud<pcl::PointXYZ>);
    pcl::PointCloud<pcl::PointXYZ>::Ptr ret_cloud (new pcl::PointCloud<pcl::PointXYZ>);


    for (int i = 0; i < 4; ++i)
    {
        std::vector<int> inliers;

        /*              Lateral thresholding            */
        // Filter the input cloud into left, right, upper and lower sections
        pcl::PointCloud<pcl::PointXYZ>::Ptr sub_cloud (new pcl::PointCloud<pcl::PointXYZ>);
        pcl::PassThrough<pcl::PointXYZ> pass;
        pass.setInputCloud(src_cloud);
        if (i < 2)
        {
            pass.setFilterFieldName("x");
            pass.setFilterLimits(thresh_range[i], thresh_range[i+1]);
        }
        else
        {
            pass.setFilterFieldName("y");
            pass.setFilterLimits(thresh_range[i+1], thresh_range[i+2]);
        }
        pass.filter(*sub_cloud);

        if (sub_cloud->size() < 5)
        {
            continue;
        }

        /*          Statistical outlier removal         */
        pcl::StatisticalOutlierRemoval<pcl::PointXYZ> sor;
        sor.setInputCloud(sub_cloud);
        sor.setMeanK(50);
        sor.setStddevMulThresh(1.0);
        sor.filter(*sub_cloud);

        /*          Split longitudinally            */
        pcl::PassThrough<pcl::PointXYZ> long_pass;
        long_pass.setInputCloud(sub_cloud);
        long_pass.setFilterFieldName("z");
        // Five wall segments for now
        for (int j = 0; j > -5; --j)
        {
            pcl::PointCloud<pcl::PointXYZ>::Ptr tmp (new pcl::PointCloud<pcl::PointXYZ>);
            long_pass.setFilterLimits(j - 1, j);
            long_pass.filter(*tmp);

            if (tmp->size() < 5)
            {
                // Empty point clouds can't have planes fitted to them
                continue;
            }

            /*          RANSAC plane fitting            */
            // Fit a plane to each wall segment
            pcl::SampleConsensusModelPlane<pcl::PointXYZ>::Ptr
                sub_plane (new pcl::SampleConsensusModelPlane<pcl::PointXYZ> (tmp));
            pcl::RandomSampleConsensus<pcl::PointXYZ> ransac (sub_plane);
            ransac.setDistanceThreshold(thresh);
            ransac.computeModel();
            ransac.getInliers(inliers);
            pcl::copyPointCloud<pcl::PointXYZ>(*tmp, inliers, *tmp_cloud);
            *ret_cloud += *tmp_cloud;
        }
    }
    return *ret_cloud;
}
