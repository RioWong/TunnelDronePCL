#include <stdexcept>
#include <iostream>

#include "stitched_cloud.h"

typedef pcl::PointXYZ PointT;
typedef pcl::PointCloud<PointT> PointCloudT;

void helpMessage();
void getTransformationData(std::ifstream& infile, std::vector<TransformData>& translation_and_rotation_raw, char delim);
void averageTransformationData(std::vector<TransformData>& transformations, const int vals_per_cloud);
void getFileList(const std::string& path, std::vector<std::string>& files);

// General outline
// - Read input
// - Find new PCD
// - Perform alignment
// - Add to stitched cloud
// - Repeat until all files completed (write to file every 5?)


// Ensure only PCD files are read
static bool filePredicate(const std::string &s)
{
    std::size_t i = s.find(".");
    return (!s.compare(".") || !s.compare("..") || !s.compare("filtered.pcd") || s.substr(i, 4).compare(".pcd"));
}

int main(int argc, char** argv)
{
    // Start the timer
    auto start = std::chrono::system_clock::now();

    /*          Handle Input        */
    // Check whether a file has been supplied
    if (argc < 3)
    {
        helpMessage();
        return -1;
    }
    // Read in the PCD filenames
    std::vector<std::string> files_to_process;
    std::string directory;
    if (!std::strcmp(argv[1], "-f"))
    {
        std::string filename = argv[2];
        int idx = filename.find_last_of("/");
        directory = filename.substr(0, idx);
        files_to_process.push_back(filename.substr(idx));
    }
    else if (!std::strcmp(argv[1], "-d"))
    {
        directory = argv[2];
        if (directory.back() != '/')
        {
            directory += "/";
        }
        getFileList(directory, files_to_process);
    }
    else
    {
        std::cout << "Command \"" << argv[1] << "\" not recognised." << std::endl;
        helpMessage();
        return -1;
    }

    // Known locations and orientations can be used to accelerate cloud processing
    std::vector<TransformData> cloud_transformations;
    if (argc > 4 && !std::strcmp(argv[3], "-t"))
    {
        std::ifstream infile(argv[4]);
        char delim = ';';
        try { getTransformationData(infile, cloud_transformations, delim); } catch (std::exception& e) { e.what(); }
        averageTransformationData(cloud_transformations, 10); // Assuming 10 rows per cloud
    }
    else
    {
        // If no file was supplied, fill the transformations vector with zeros
        for (int i = 0; i < files_to_process.size(); ++i)
        {
            TransformData t;
            cloud_transformations.push_back(t);
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

    // Sort files_to_process into ascending order
    struct compareFileNamesStruct
    {
        bool operator() (std::string a, std::string b)
        {
            size_t a_idx_start = a.find_last_of("D") + 1;
            size_t a_idx_end = a.find(".");
            int a_int = stoi(a.substr(a_idx_start, a_idx_end - a_idx_start));

            size_t b_idx_start = b.find_last_of("D") + 1;
            size_t b_idx_end = b.find(".");
            int b_int = stoi(b.substr(b_idx_start, b_idx_end - b_idx_start));

            return a_int < b_int;
        }
    } compareFilenames;
    std::sort(files_to_process.begin(), files_to_process.end(), compareFilenames);

    /*          Process Point Clouds                */
    PointCloudT::Ptr first_cloud (new PointCloudT());
    std::string path = directory + files_to_process[0];
    pcl::PCDReader reader;
    reader.read(path, *first_cloud);
    StitchedCloud stitchedCloud (first_cloud);

    // TODO Set the first point cloud as being centre at the origin (translation by cloud_transformations[0])
    // Not coded yet - just planning

    // Display a progress bar
    boost::progress_display progress_bar(files_to_process.size()-1);

    // Add each new cloud to the stitched_cloud
    PointCloudT::Ptr new_cloud (new PointCloudT());
    for (int i = 1; i < files_to_process.size(); ++i)
    {
        path = directory + files_to_process[i];
        reader.read(path, *new_cloud);
        // Remove any NaNs to improve speed
        std::vector<int> tmp;
        pcl::removeNaNFromPointCloud(*new_cloud, *new_cloud, tmp);
        // Add this cloud to the stitched cloud
        stitchedCloud.addCloud(new_cloud, cloud_transformations[i] - cloud_transformations[0]);
        ++progress_bar;
    }

    // Reconstruct the surface
    // pcl::PointCloud<pcl::PointNormal>::Ptr mls_points (new pcl::PointCloud<pcl::PointNormal>());
    // reconstructSurface(mls_points, stitchedCloud.stitched_cloud, 500);

    // Write the resulting point cloud
    pcl::io::savePCDFile(directory + "/filtered.pcd", *(stitchedCloud.stitched_cloud));
    // pcl::io::savePCDFile(directory + "/filtered.pcd", *mls_points);

    // Timing
    auto end = std::chrono::system_clock::now();
    stitchedCloud.timeBreakdown.total_time = std::chrono::duration_cast<std::chrono::milliseconds>(end - start)/1000.0;
    stitchedCloud.timeBreakdown.print();

    return 0;
}

void helpMessage()
{
    std::cout << "Usage:\n"
              << "\t-f <file>" << "\t\tProcess a single file.\n"
              << "\t-d <directory>" << "\t\tProcess all of the pcd files in a directory.\n"
              << "\t-t <txt file>" << "\t\tSupply translation and rotation information. (OPTIONAL)"
              << std::endl;
}

void getTransformationData(std::ifstream& infile, std::vector<TransformData>& translation_and_rotation_raw, char delim)
{
    // Read transformation information from a supplied file
    // Assumes a one-to-one correspondence between data and pcd files
    // In the order dx, dy, dz, rotx, roty, rotz

    std::string line;
    const int rows_to_skip = 1;
    const int cols_to_skip = 1;
    int row = -1;
    while (std::getline(infile, line))
    {
        // Skip header rows
        if (rows_to_skip > ++row)
        {
            continue;
        }

        std::vector<double> transformVec;
        std::istringstream iss (line);
        std::string val = "0.0";
        int col = 0;
        while (std::getline(iss, val, delim))
        {
            if (cols_to_skip > col++)
            {
                continue;
            }
            transformVec.push_back(stod(val));
        }

        TransformData t_data;
        if (transformVec.size() < 6)
            throw std::domain_error("Input transformations file has fewer than 6 columns.");
        t_data.rotx = transformVec[0];
        t_data.roty = transformVec[1];
        t_data.rotz = transformVec[2];
        t_data.dx = transformVec[3];
        t_data.dy = transformVec[4];
        t_data.dz = transformVec[5];
        if (transformVec.size() > 6)
            t_data.confidence = transformVec[6];
        else
            std::cout << "WARNING: No confidence value given for transformation data.\n";
        translation_and_rotation_raw.push_back(t_data);
    }
}

void averageTransformationData(std::vector<TransformData>& transformations, const int vals_per_cloud)
{
    // Take the average value of each translation and rotation over vals_per_cloud elements
    for (int i = 0; i < transformations.size() + vals_per_cloud; i += vals_per_cloud)
    {
        for (int j = 1; j < vals_per_cloud; ++j)
        {
            transformations[i].dx += transformations[i+j].dx;
            transformations[i].dy += transformations[i+j].dy;
            transformations[i].dz += transformations[i+j].dz;
            transformations[i].rotx += transformations[i+j].rotx;
            transformations[i].roty += transformations[i+j].roty;
            transformations[i].rotz += transformations[i+j].rotz;
            transformations[i].confidence += transformations[i+j].confidence;
        }
        transformations[i].dx /= vals_per_cloud;
        transformations[i].dy /= vals_per_cloud;
        transformations[i].dz /= vals_per_cloud;
        transformations[i].rotx /= vals_per_cloud;
        transformations[i].roty /= vals_per_cloud;
        transformations[i].rotz /= vals_per_cloud;
        transformations[i].confidence /= vals_per_cloud;
    }

    int i = 0;
    while (i < transformations.size())
    {
        for (int j = 0; j < vals_per_cloud; ++j)
        {
            transformations.erase(transformations.begin() + i + 1);
        }
        ++i;
    }
}

void getFileList(const std::string& path, std::vector<std::string>& files)
{
    DIR *dp;
    struct dirent *dirp;
    if((dp  = opendir(path.c_str())) == NULL) {
        std::cout << "Error opening " << path << std::endl;
        return;
    }

    while ((dirp = readdir(dp)) != NULL) {
        files.push_back(std::string(dirp->d_name));
    }
    closedir(dp);
}
